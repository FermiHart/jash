#!/usr/bin/env python3
"""Maintenance binds intent/results and releases retention, never execution history."""
import json
import subprocess
import unittest
import shutil
import os
import shlex
import time
import tempfile
import concurrent.futures
import threading
from pathlib import Path
import test_agent_artifact_store as fixture


class Transactions(unittest.TestCase):
    setUp = fixture.Store.setUp
    setUpClass = classmethod(fixture.Store.setUpClass.__func__)
    request = fixture.Store.request
    produce = fixture.Store.produce
    orphan = fixture.Store.orphan

    def execute(self, request):
        wire = (json.dumps(request) + "\n").encode()
        p = subprocess.run([str(fixture.AGENT)], input=wire, cwd=self.work, env=self.env,
                           capture_output=True, timeout=20)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.runtime_key = json.loads(p.stdout.splitlines()[0])["receipts"]["pubkey"]
        raw = p.stdout.splitlines(keepends=True)[-1]
        return json.loads(raw), wire, raw

    def test_quota_receipt_is_bound_and_retry_replays_without_reapplying(self):
        request = {"id": "quota-once", "protocol_version": 2, "op": "store.quota", "max_bytes": 100000,
                   "caps": ["store:manage"]}
        result, wire, raw = self.execute(request)
        self.assertIn("receipt_ref", result, result)
        self.assertEqual(result["transaction"], "committed")
        req = self.work / "request.json"; req.write_bytes(wire)
        res = self.work / "response.json"; res.write_bytes(raw)
        receipt = self.home / ".jash/agent_maintenance" / result["receipt_ref"]
        self.assertEqual(result["receipt_pubkey"], self.runtime_key)
        verified = subprocess.run([str(fixture.fixture.ROOT / "build/jef_verify"), "maintenance-receipt", self.runtime_key,
            str(req), str(res), str(receipt)], capture_output=True, timeout=10)
        self.assertEqual(verified.returncode, 0, (verified.stdout, verified.stderr))
        self.assertTrue(self.request("store.quota", max_bytes=200000)["ok"])
        replay, _, replay_raw = self.execute(request)
        self.assertEqual(replay_raw, raw)
        self.assertEqual(self.request("store.inspect")["quota_bytes"], 200000)
        changed = dict(request, max_bytes=300000)
        rejected, _, _ = self.execute(changed)
        self.assertEqual(rejected["error"], "maintenance_id_conflict")

    def test_release_all_references_allows_gc_but_keeps_execution_evidence(self):
        produced = self.produce()
        target = self.home / ".jash/overflow" / produced["stdout_ref"]
        inspected = self.request("store.inspect")
        roots = [item for item in inspected["items"] if item["kind"] in ("job_archive", "receipt", "idempotency")]
        evidence = {self.home / ".jash" / item["namespace"] / item["name"]:
                    (self.home / ".jash" / item["namespace"] / item["name"]).read_bytes() for item in roots}
        archive = next(item for item in roots if item["kind"] == "job_archive")
        released = self.request("store.release", root_namespace=archive["namespace"], root_name=archive["name"], root_blake3=archive["root_blake3"])
        self.assertTrue(released["ok"], released)
        self.assertTrue(self.request("store.gc", older_than_seconds=0)["ok"])
        self.assertTrue(target.exists(), "other retained roots still protect shared output")
        for item in roots:
            if item == archive: continue
            result = self.request("store.release", root_namespace=item["namespace"], root_name=item["name"], root_blake3=item["root_blake3"])
            self.assertTrue(result["ok"], result)
        collected = self.request("store.gc", older_than_seconds=0)
        self.assertTrue(collected["ok"], collected); self.assertFalse(target.exists())
        self.assertEqual(evidence, {path: path.read_bytes() for path in evidence})

    def test_release_requires_matching_version_and_preview_has_no_effect(self):
        self.produce()
        root = next(item for item in self.request("store.inspect")["items"] if item["kind"] == "job_archive")
        fields = dict(root_namespace=root["namespace"], root_name=root["name"], root_blake3=root["root_blake3"])
        preview = self.request("store.release", **fields, dry_run=True)
        self.assertTrue(preview["ok"]); self.assertFalse(preview["applied"])
        stale = self.request("store.release", **dict(fields, root_blake3="0" * 64))
        self.assertEqual(stale["error"], "root_changed")
        inspected = self.request("store.inspect")
        self.assertTrue(all(item["retention"] != "released" for item in inspected["items"]))

    def test_gc_retry_does_not_delete_a_new_object_at_the_same_name(self):
        self.request("store.inspect"); orphan = self.orphan()
        request = {"id": "gc-once", "protocol_version": 2, "op": "store.gc", "older_than_seconds": 0, "caps": ["store:manage"]}
        first, _, raw = self.execute(request)
        self.assertTrue(first["ok"]); self.assertFalse(orphan.exists())
        self.orphan()
        _, _, replay = self.execute(request)
        self.assertEqual(replay, raw); self.assertTrue(orphan.exists())

    def test_interrupted_commit_blocks_reexecution_and_later_mutation(self):
        self.env.update(LD_PRELOAD=self.hook, JASH_TEST_MAINT_COMMIT_FAIL="1")
        request = {"id": "interrupted", "protocol_version": 2, "op": "store.quota", "max_bytes": 100000, "caps": ["store:manage"]}
        failed, _, _ = self.execute(request)
        self.assertEqual(failed["transaction"], "indeterminate"); self.assertTrue(failed["applied"])
        self.env.pop("LD_PRELOAD")
        retry, _, _ = self.execute(request)
        self.assertEqual(retry["error"], "maintenance_indeterminate")
        rejected = self.request("store.quota", max_bytes=200000)
        self.assertEqual(rejected["error"], "maintenance_incomplete")
        observed = self.request("store.inspect")
        self.assertEqual(observed["quota_bytes"], 100000)

    def test_independent_verifier_rejects_changed_request_response_or_intent(self):
        request = {"id": "binding", "protocol_version": 2, "op": "store.quota", "max_bytes": 100000, "caps": ["store:manage"]}
        result, wire, raw = self.execute(request)
        req = self.work / "request"; req.write_bytes(wire)
        res = self.work / "response"; res.write_bytes(raw)
        receipt = self.home / ".jash/agent_maintenance" / result["receipt_ref"]
        command = [str(fixture.fixture.ROOT / "build/jef_verify"), "maintenance-receipt", result["receipt_pubkey"], str(req), str(res), str(receipt)]
        req.write_bytes(wire.replace(b"100000", b"200000"))
        self.assertNotEqual(subprocess.run(command, capture_output=True, timeout=10).returncode, 0)
        req.write_bytes(wire); res.write_bytes(raw.replace(b'"applied":true', b'"applied":false'))
        self.assertNotEqual(subprocess.run(command, capture_output=True, timeout=10).returncode, 0)
        res.write_bytes(raw)
        intent = next(receipt.parent.glob("*.maintenance-intent.dsse")); intent.write_text("{}")
        self.assertNotEqual(subprocess.run(command, capture_output=True, timeout=10).returncode, 0)

    def test_release_does_not_override_recovery_pins(self):
        produced = self.produce(); target = self.home / ".jash/overflow" / produced["stdout_ref"]
        source = self.work / "pin-source"; source.write_bytes(target.read_bytes())
        p = subprocess.run([str(fixture.fixture.JASH), "-c", shlex.join(["cp", "--json", str(source), str(target)])],
                           cwd=self.work, env=dict(self.env, JASH_TIMELINE="1"), capture_output=True, timeout=20)
        self.assertEqual(p.returncode, 0, p.stderr); self.assertIsNotNone(json.loads(p.stdout)["items"][0]["undo"])
        for root in self.request("store.inspect")["items"]:
            if root["root_blake3"]:
                result = self.request("store.release", root_namespace=root["namespace"], root_name=root["name"], root_blake3=root["root_blake3"])
                self.assertTrue(result["ok"], result)
        self.assertTrue(self.request("store.gc", older_than_seconds=0)["ok"])
        self.assertTrue(target.exists())

    def test_interrupted_release_never_authorizes_gc(self):
        produced = self.produce(); target = self.home / ".jash/overflow" / produced["stdout_ref"]
        root = next(item for item in self.request("store.inspect")["items"] if item["kind"] == "job_archive")
        self.env.update(LD_PRELOAD=self.hook, JASH_TEST_MAINT_COMMIT_FAIL="1")
        failed = self.request("store.release", root_namespace=root["namespace"], root_name=root["name"], root_blake3=root["root_blake3"])
        self.assertEqual(failed["transaction"], "indeterminate")
        self.env.pop("LD_PRELOAD")
        blocked = self.request("store.gc", older_than_seconds=0)
        self.assertEqual(blocked["error"], "maintenance_incomplete"); self.assertTrue(target.exists())

    def test_killed_after_effect_does_not_reexecute(self):
        marker = self.work / "commit-paused"
        request = {"id": "killed", "protocol_version": 2, "op": "store.quota", "max_bytes": 123456, "caps": ["store:manage"]}
        env = dict(self.env, LD_PRELOAD=self.hook, JASH_TEST_MAINT_COMMIT_GATE=str(marker))
        proc = subprocess.Popen([str(fixture.AGENT)], cwd=self.work, env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            json.loads(proc.stdout.readline())
            proc.stdin.write((json.dumps(request) + "\n").encode()); proc.stdin.flush()
            deadline = time.monotonic() + 10
            while not marker.exists() and proc.poll() is None and time.monotonic() < deadline: time.sleep(.01)
            self.assertTrue(marker.exists())
            self.assertEqual((self.home / ".jash/agent_store/quota").read_text(), "123456\n")
        finally:
            proc.kill(); proc.communicate(timeout=10)
        retried, _, _ = self.execute(request)
        self.assertEqual(retried["error"], "maintenance_indeterminate")
        self.assertEqual(self.request("store.inspect")["quota_bytes"], 123456)

    def test_missing_intent_is_not_treated_as_fresh_history(self):
        result = self.request("store.quota", max_bytes=123456)
        self.assertTrue(result["ok"])
        next((self.home / ".jash/agent_maintenance").glob("*.intent")).unlink()
        self.assertEqual(self.request("store.quota", max_bytes=999999)["error"], "maintenance_incomplete")

    def test_lost_terminal_delivery_replays_the_committed_result(self):
        marker = self.work / "published"
        request = {"id": "lost-delivery", "protocol_version": 2, "op": "store.quota", "max_bytes": 123456, "caps": ["store:manage"]}
        env = dict(self.env, LD_PRELOAD=self.hook, JASH_TEST_MAINT_COMMIT_GATE=str(marker))
        proc = subprocess.Popen([str(fixture.AGENT)], cwd=self.work, env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            json.loads(proc.stdout.readline())
            proc.stdin.write((json.dumps(request) + "\n").encode()); proc.stdin.flush()
            deadline = time.monotonic() + 10
            while not marker.exists() and proc.poll() is None and time.monotonic() < deadline: time.sleep(.01)
            self.assertTrue(marker.exists())
            proc.stdout.close(); proc.stdout = None; proc.stdin.close(); proc.stdin = None
            Path(str(marker) + ".release").touch()
            proc.communicate(timeout=10)
        finally:
            if proc.poll() is None: proc.kill(); proc.communicate(timeout=10)
        replay, _, _ = self.execute(request)
        self.assertEqual(replay["transaction"], "committed"); self.assertTrue(replay["applied"])

    def test_busy_refusal_does_not_reserve_the_id(self):
        request = {"id": "retry-busy", "protocol_version": 2, "op": "store.quota", "max_bytes": 123456, "caps": ["store:manage"]}
        peer = subprocess.Popen([str(fixture.AGENT)], cwd=self.work, env=self.env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            json.loads(peer.stdout.readline())
            refused, _, _ = self.execute(request)
            self.assertEqual(refused["error"], "store_busy"); self.assertEqual(refused["transaction"], "not_started")
        finally:
            peer.stdin.close(); peer.stdin = None; peer.communicate(timeout=10)
        result, _, _ = self.execute(request)
        self.assertEqual(result["transaction"], "committed")

    def test_failed_intent_sync_precedes_policy_effect(self):
        request = {"id": "prepare-failure", "protocol_version": 2, "op": "store.quota", "max_bytes": 123456, "caps": ["store:manage"]}
        self.env.update(LD_PRELOAD=self.hook, JASH_TEST_MAINT_PREPARE_FAIL="1")
        failed, _, _ = self.execute(request)
        self.assertEqual(failed["error"], "maintenance_prepare_failed")
        self.assertFalse(failed["applied"]); self.assertEqual(failed["quota_bytes"], 0)
        self.assertFalse((self.home / ".jash/agent_store/quota").exists())
        self.env.pop("LD_PRELOAD")
        self.assertEqual(self.execute(request)[0]["transaction"], "committed")

    def test_release_does_not_allow_original_execution_to_run_again(self):
        effect = self.work / "effect"
        request = {"id": "execute-once", "protocol_version": 2, "op": "exec", "argv": ["/usr/bin/python3", "-c",
            "open('effect','ab').write(b'x'); print('data')"], "caps": ["effect:exec", "job:create"]}
        produced, _, _ = self.execute(request)
        self.assertEqual(effect.read_bytes(), b"x")
        for root in self.request("store.inspect")["items"]:
            if root["root_blake3"]:
                self.assertTrue(self.request("store.release", root_namespace=root["namespace"], root_name=root["name"], root_blake3=root["root_blake3"])["ok"])
        self.assertTrue(self.request("store.gc", older_than_seconds=0)["ok"])
        rejected, _, _ = self.execute(request)
        self.assertIn("error", rejected)
        self.assertEqual(effect.read_bytes(), b"x")
        view, _, _ = self.execute({"id": "view", "protocol_version": 2, "op": "job.result", "job_id": produced["job_id"], "caps": ["job:read"]})
        self.assertTrue(view["available"]); self.assertEqual(view["retention"], "released")


class Resolution(unittest.TestCase):
    setUp = fixture.Store.setUp
    setUpClass = classmethod(fixture.Store.setUpClass.__func__)
    request = fixture.Store.request
    execute = Transactions.execute
    orphan = fixture.Store.orphan
    produce = fixture.Store.produce

    def interrupted(self, operation="store.quota", identifier="pending", **fields):
        request = {"id": identifier, "protocol_version": 2, "op": operation,
                   "caps": ["store:manage"], **fields}
        if operation == "store.quota": request.setdefault("max_bytes", 123456)
        self.env.update(LD_PRELOAD=self.hook, JASH_TEST_MAINT_COMMIT_FAIL="1")
        try:
            failed, wire, _ = self.execute(request)
        finally:
            self.env.pop("LD_PRELOAD"); self.env.pop("JASH_TEST_MAINT_COMMIT_FAIL")
        self.assertEqual(failed["transaction"], "indeterminate")
        directory = self.home / ".jash/agent_maintenance"
        candidate = next(directory.glob(".*.result.*.tmp"))
        return request, wire, candidate

    def test_resolution_publishes_verified_result_without_reapplying_quota(self):
        original, wire, candidate = self.interrupted()
        raw = candidate.read_bytes()
        resolved = self.request("store.resolve", target_id=original["id"])
        self.assertEqual(resolved.get("schema"), "jash.agent-maintenance-resolution.v1", resolved)
        self.assertEqual(resolved["status"], "recovered"); self.assertTrue(resolved["published"])
        self.assertTrue(resolved["maintenance_complete"])
        self.assertEqual(candidate.read_bytes(), raw)
        result = self.home / ".jash/agent_maintenance" / resolved["result_ref"]
        self.assertEqual(result.read_bytes(), raw)
        self.assertTrue(self.request("store.quota", max_bytes=999999)["ok"])
        replay, _, reply = self.execute(original)
        self.assertEqual(reply, raw); self.assertEqual(replay["transaction"], "committed")
        self.assertEqual(self.request("store.inspect")["quota_bytes"], 999999)
        again = self.request("store.resolve", target_id=original["id"])
        self.assertEqual(again["status"], "already_committed"); self.assertFalse(again["published"])
        req = self.work / "request"; req.write_bytes(wire)
        receipt = result.parent / replay["receipt_ref"]
        checked = subprocess.run([str(fixture.fixture.ROOT / "build/jef_verify"), "maintenance-receipt", self.runtime_key,
                                  str(req), str(result), str(receipt)], capture_output=True, timeout=10)
        self.assertEqual(checked.returncode, 0, checked.stderr)


    def test_preview_preserves_journal_and_blocked_state(self):
        original, _, candidate = self.interrupted()
        before = {p.name: p.read_bytes() for p in candidate.parent.iterdir()}
        preview = self.request("store.resolve", target_id=original["id"], dry_run=True)
        self.assertEqual(preview["status"], "recoverable"); self.assertFalse(preview["published"])
        self.assertFalse(preview["maintenance_complete"])
        self.assertEqual(before, {p.name: p.read_bytes() for p in candidate.parent.iterdir()})
        self.assertEqual(self.execute(original)[0]["error"], "maintenance_indeterminate")

    def test_gc_resolution_preserves_recreated_object(self):
        self.request("store.inspect"); orphan = self.orphan()
        original, _, candidate = self.interrupted("store.gc", older_than_seconds=0)
        self.assertFalse(orphan.exists()); self.orphan()
        raw = candidate.read_bytes()
        self.assertTrue(self.request("store.resolve", target_id=original["id"])["ok"])
        self.assertTrue(orphan.exists()); self.assertEqual(self.execute(original)[2], raw)
        self.assertTrue(orphan.exists())

    def test_recovered_failed_verdict_is_not_converted_to_operation_success(self):
        self.request("store.inspect"); self.orphan()
        original, _, candidate = self.interrupted(max_bytes=1)
        resolved = self.request("store.resolve", target_id=original["id"])
        self.assertTrue(resolved["ok"]); self.assertEqual(resolved["status"], "recovered")
        replay, _, raw = self.execute(original)
        self.assertFalse(replay["ok"]); self.assertFalse(replay["applied"])
        self.assertEqual(replay["error"], "quota_below_usage"); self.assertEqual(raw, candidate.read_bytes())

    def test_release_becomes_effective_only_after_verified_resolution(self):
        produced = self.produce(); target = self.home / ".jash/overflow" / produced["stdout_ref"]
        root = next(item for item in self.request("store.inspect")["items"] if item["kind"] == "job_archive")
        original, _, candidate = self.interrupted("store.release", root_namespace=root["namespace"],
                                                 root_name=root["name"], root_blake3=root["root_blake3"])
        self.assertEqual(self.request("store.gc", older_than_seconds=0)["error"], "maintenance_incomplete")
        self.assertTrue(self.request("store.resolve", target_id=original["id"])["ok"])
        roots = self.request("store.inspect")["items"]
        self.assertEqual(next(item for item in roots if item["name"] == root["name"])["retention"], "released")
        self.assertTrue(self.request("store.gc", older_than_seconds=0)["ok"]); self.assertTrue(target.exists())
        for entry in roots:
            if entry["root_blake3"] and entry["retention"] != "released":
                self.assertTrue(self.request("store.release", root_namespace=entry["namespace"],
                                             root_name=entry["name"], root_blake3=entry["root_blake3"])["ok"])
        self.assertTrue(self.request("store.gc", older_than_seconds=0)["ok"]); self.assertFalse(target.exists())
        self.assertEqual(self.execute(original)[2], candidate.read_bytes())

    def test_invalid_candidate_or_intent_receipt_never_publishes(self):
        original, _, candidate = self.interrupted()
        raw = candidate.read_bytes(); intent = next(candidate.parent.glob("*.maintenance-intent.dsse"))
        proof = intent.read_bytes()
        for corruption in ("response", "intent", "fifo", "symlink", "permissions", "oversize"):
            with self.subTest(corruption=corruption):
                candidate.unlink(); candidate.write_bytes(raw); candidate.chmod(0o600); intent.write_bytes(proof)
                if corruption == "response": candidate.write_bytes(raw.replace(b'"applied":true', b'"applied":false'))
                elif corruption == "intent": intent.write_text("{}")
                elif corruption == "fifo": candidate.unlink(); os.mkfifo(candidate)
                elif corruption == "symlink":
                    copy = self.work / "copy"; copy.write_bytes(raw); candidate.unlink(); candidate.symlink_to(copy)
                elif corruption == "permissions": candidate.chmod(0o644)
                else:
                    with candidate.open("wb") as stream: stream.truncate(16 * 1024 * 1024 + 1)
                rejected = self.request("store.resolve", target_id=original["id"])
                self.assertFalse(rejected["ok"]); self.assertFalse(rejected["published"])
                self.assertFalse(list(candidate.parent.glob("*.result")))

    def test_corrupt_public_result_is_not_overwritten_from_valid_staging(self):
        original, _, candidate = self.interrupted()
        public = candidate.parent / (candidate.name[1:65] + ".result")
        public.write_bytes(b"corrupted"); public.chmod(0o600)
        refused = self.request("store.resolve", target_id=original["id"])
        self.assertEqual(refused["error"], "result_invalid"); self.assertEqual(public.read_bytes(), b"corrupted")

    def test_missing_terminal_retains_ambiguity_and_unique_data(self):
        self.request("store.inspect"); orphan = self.orphan()
        original, _, candidate = self.interrupted()
        candidate.unlink()
        refused = self.request("store.resolve", target_id=original["id"])
        self.assertEqual(refused["error"], "terminal_unavailable")
        self.assertEqual(self.request("store.gc", older_than_seconds=0)["error"], "maintenance_incomplete")
        self.assertTrue(orphan.exists()); self.assertEqual(self.request("store.inspect")["quota_bytes"], 123456)

    def test_other_pending_transaction_keeps_maintenance_blocked(self):
        original, _, candidate = self.interrupted()
        intent = next(candidate.parent.glob("*.intent"))
        extra = candidate.parent / ("a" * 64 + ".intent"); extra.write_bytes(intent.read_bytes()); extra.chmod(0o600)
        resolved = self.request("store.resolve", target_id=original["id"])
        self.assertTrue(resolved["ok"]); self.assertFalse(resolved["maintenance_complete"])
        self.assertEqual(self.request("store.gc", older_than_seconds=0)["error"], "maintenance_incomplete")

    def test_scan_limit_does_not_publish_a_valid_prefix(self):
        original, _, candidate = self.interrupted()
        for i in range(4097): (candidate.parent / f".extra-{i}").touch(mode=0o600)
        refused = self.request("store.resolve", target_id=original["id"])
        self.assertEqual(refused["error"], "scan_limit"); self.assertFalse(refused["published"])
        self.assertFalse(list(candidate.parent.glob("*.result")))

    def test_conflicting_signed_candidates_are_not_arbitrarily_selected(self):
        original, _, candidate = self.interrupted()
        directory = candidate.parent; saved = self.work / "saved-journal"
        directory.rename(saved)
        # Construct two genuine signed results for the identical request with
        # different observed stores. This deliberately bypasses the journal in
        # the fixture only; recovery must refuse either possible ordering.
        self.orphan()
        self.interrupted()
        for path in saved.iterdir():
            destination = directory / path.name
            if not destination.exists(): shutil.copyfile(path, destination); destination.chmod(0o600)
        refused = self.request("store.resolve", target_id=original["id"])
        self.assertEqual(refused["error"], "candidate_conflict")
        self.assertFalse(list(directory.glob("*.result")))

    def test_target_identity_is_exact_and_missing_intent_is_not_reconstructed(self):
        original, _, candidate = self.interrupted(identifier='"' * 47)
        wrong = self.request("store.resolve", target_id="different")
        self.assertEqual(wrong["error"], "intent_missing")
        self.assertTrue(self.request("store.resolve", target_id=original["id"])["ok"])
        self.assertEqual(self.execute(original)[2], candidate.read_bytes())
        next(candidate.parent.glob("*.intent")).unlink()
        refused = self.request("store.resolve", target_id=original["id"])
        self.assertEqual(refused["error"], "intent_missing"); self.assertFalse(refused["maintenance_complete"])

    def kill_at(self, request, fault):
        marker = self.work / (fault + "-ready")
        env = dict(self.env, LD_PRELOAD=self.hook, **{fault: str(marker)})
        proc = subprocess.Popen([str(fixture.AGENT)], cwd=self.work, env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            json.loads(proc.stdout.readline()); proc.stdin.write((json.dumps(request) + "\n").encode()); proc.stdin.flush()
            deadline = time.monotonic() + 10
            while not marker.exists() and proc.poll() is None and time.monotonic() < deadline: time.sleep(.01)
            self.assertTrue(marker.exists())
        finally:
            proc.kill(); proc.communicate(timeout=10)

    def test_killed_before_effect_stays_unresolved(self):
        original = {"id": "before", "protocol_version": 2, "op": "store.quota", "max_bytes": 123456, "caps": ["store:manage"]}
        self.kill_at(original, "JASH_TEST_MAINT_INTENT_GATE")
        self.assertFalse((self.home / ".jash/agent_store/quota").exists())
        refused = self.request("store.resolve", target_id=original["id"])
        self.assertEqual(refused["error"], "terminal_unavailable")
        self.assertEqual(self.execute(original)[0]["error"], "maintenance_indeterminate")
        self.assertFalse((self.home / ".jash/agent_store/quota").exists())

    def test_killed_after_effect_and_during_resolution_recovers_once(self):
        original = {"id": "after", "protocol_version": 2, "op": "store.quota", "max_bytes": 123456, "caps": ["store:manage"]}
        self.kill_at(original, "JASH_TEST_MAINT_COMMIT_GATE")
        candidate = next((self.home / ".jash/agent_maintenance").glob(".*.result.*.tmp")); raw = candidate.read_bytes()
        # A second interrupted publication produces identical candidates, not a
        # second policy application. Recovery must tolerate that evidence.
        gate = self.work / "JASH_TEST_MAINT_COMMIT_GATE-ready"; gate.unlink()
        resolve = {"id": "resolve", "protocol_version": 2, "op": "store.resolve", "target_id": original["id"], "caps": ["store:manage"]}
        self.kill_at(resolve, "JASH_TEST_MAINT_COMMIT_GATE")
        self.assertEqual(len(list(candidate.parent.glob(".*.result.*.tmp"))), 2)
        self.assertTrue(self.request("store.resolve", target_id=original["id"])["ok"])
        self.assertEqual(self.execute(original)[2], raw)

    def test_publish_failure_is_retryable_and_retains_evidence(self):
        original, _, candidate = self.interrupted()
        self.env.update(LD_PRELOAD=self.hook, JASH_TEST_MAINT_COMMIT_FAIL="1")
        failed = self.request("store.resolve", target_id=original["id"])
        self.assertEqual(failed["error"], "publication_unconfirmed"); self.assertFalse(failed["published"])
        self.assertTrue(candidate.exists())
        self.env.pop("LD_PRELOAD"); self.env.pop("JASH_TEST_MAINT_COMMIT_FAIL")
        self.assertTrue(self.request("store.resolve", target_id=original["id"])["ok"])

    def test_directory_sync_failure_reports_visible_result_and_retries_sync(self):
        original, _, candidate = self.interrupted()
        self.env.update(LD_PRELOAD=self.hook, JASH_TEST_MAINT_DIRSYNC_FAIL="1")
        failed = self.request("store.resolve", target_id=original["id"])
        self.assertEqual(failed["error"], "publication_unconfirmed"); self.assertTrue(failed["published"])
        self.assertEqual((candidate.parent / failed["result_ref"]).read_bytes(), candidate.read_bytes())
        again = self.request("store.resolve", target_id=original["id"])
        self.assertEqual(again["error"], "publication_unconfirmed"); self.assertFalse(again["published"])
        self.env.pop("LD_PRELOAD"); self.env.pop("JASH_TEST_MAINT_DIRSYNC_FAIL")
        self.assertEqual(self.request("store.resolve", target_id=original["id"])["status"], "already_committed")

    def test_busy_and_invalid_requests_precede_publication(self):
        original, _, candidate = self.interrupted()
        request = {"id": "resolve", "protocol_version": 2, "op": "store.resolve", "target_id": original["id"], "caps": ["store:manage"]}
        for invalid in (dict(request, caps=["store:read"]), dict(request, target_id=""), dict(request, max_bytes=0), dict(request, target_id="x" * 48)):
            self.assertIn("error", self.execute(invalid)[0]); self.assertFalse(list(candidate.parent.glob("*.result")))
        peer = subprocess.Popen([str(fixture.AGENT)], cwd=self.work, env=self.env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            json.loads(peer.stdout.readline())
            self.assertEqual(self.execute(request)[0]["error"], "store_busy")
            self.assertFalse(list(candidate.parent.glob("*.result")))
        finally:
            peer.stdin.close(); peer.stdin = None; peer.communicate(timeout=10)

    def test_closed_response_header_prevents_publication(self):
        original, _, candidate = self.interrupted()
        request = {"id": "resolve", "protocol_version": 2, "op": "store.resolve", "target_id": original["id"], "caps": ["store:manage"]}
        proc = subprocess.Popen([str(fixture.AGENT)], cwd=self.work, env=self.env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            json.loads(proc.stdout.readline()); proc.stdout.close(); proc.stdout = None
            proc.communicate((json.dumps(request) + "\n").encode(), timeout=10)
        finally:
            if proc.poll() is None: proc.kill(); proc.communicate(timeout=10)
        self.assertFalse(list(candidate.parent.glob("*.result"))); self.assertTrue(candidate.exists())


class JournalGC(unittest.TestCase):
    setUp = fixture.Store.setUp
    setUpClass = classmethod(fixture.Store.setUpClass.__func__)
    execute = Transactions.execute
    request = fixture.Store.request
    interrupted = Resolution.interrupted
    kill_at = Resolution.kill_at

    def redundant(self):
        original, _, candidate = self.interrupted()
        self.assertTrue(self.request("store.resolve", target_id=original["id"])["ok"])
        return original, candidate

    def test_preview_and_gc_remove_only_verified_redundant_result_staging(self):
        original, candidate = self.redundant(); directory = candidate.parent
        public = {p.name: p.read_bytes() for p in directory.iterdir() if not p.name.startswith(".")}
        unique = directory / ".unique-recovery"; unique.write_bytes(b"unique"); unique.chmod(0o600)
        preview = self.request("store.journal.gc", older_than_seconds=0, dry_run=True)
        self.assertTrue(preview.get("ok"), preview)
        self.assertEqual(preview["journal_gc"]["planned_files"], 1); self.assertTrue(candidate.exists())
        result = self.request("store.journal.gc", older_than_seconds=0)
        self.assertTrue(result["ok"], result); self.assertEqual(result["journal_gc"]["reclaimed_files"], 1)
        self.assertFalse(candidate.exists()); self.assertEqual(unique.read_bytes(), b"unique")
        self.assertEqual(public, {name: (directory / name).read_bytes() for name in public})
        self.assertEqual(self.execute(original)[0]["transaction"], "committed")

    def collect(self, **fields):
        return self.request("store.journal.gc", older_than_seconds=0, **fields)

    def test_unlink_failure_retains_hold_and_new_id_reconciles(self):
        _, candidate = self.redundant(); raw = candidate.read_bytes()
        self.env.update(LD_PRELOAD=self.hook, JASH_TEST_JOURNAL_GC_UNLINK_FAIL="1")
        request = {"id": "unlink-fail", "protocol_version": 2, "op": "store.journal.gc", "older_than_seconds": 0, "caps": ["store:manage"]}
        failed, _, reply = self.execute(request)
        self.env.pop("LD_PRELOAD"); self.env.pop("JASH_TEST_JOURNAL_GC_UNLINK_FAIL")
        self.assertFalse(failed["ok"]); self.assertTrue(failed["applied"])
        item = failed["journal_gc"]["items"][0]; self.assertEqual(item["action"], "unlink_failed")
        hold = candidate.parent / item["retained_as"]; self.assertEqual(hold.read_bytes(), raw)
        self.assertEqual(self.execute(request)[2], reply); self.assertTrue(hold.exists())
        final = self.collect(); self.assertTrue(final["ok"]); self.assertEqual(final["journal_gc"]["reclaimed_files"], 1)
        self.assertFalse(hold.exists())

    def test_sigkill_after_capture_allows_reconciliation_without_forging_old_terminal(self):
        original, candidate = self.redundant(); raw = candidate.read_bytes()
        request = {"id": "killed-gc", "protocol_version": 2, "op": "store.journal.gc", "older_than_seconds": 0, "caps": ["store:manage"]}
        self.kill_at(request, "JASH_TEST_JOURNAL_GC_CAPTURE_GATE")
        hold = next(candidate.parent.glob(".jgc-*")); self.assertEqual(hold.read_bytes(), raw)
        view = self.request("store.inspect", target_id=original["id"])
        self.assertIn("result_gc_hold", [entry["role"] for entry in view["files"]])
        recovered = self.collect()
        self.assertTrue(recovered["ok"]); self.assertFalse(recovered["maintenance_complete"])
        self.assertFalse(hold.exists()); self.assertEqual(recovered["journal_gc"]["reclaimed_files"], 1)
        self.assertEqual(self.execute(request)[0]["error"], "maintenance_indeterminate")
        self.assertEqual(self.request("store.quota", max_bytes=0)["error"], "maintenance_incomplete")
        self.assertEqual(self.execute(original)[0]["transaction"], "committed")

    def test_missing_or_changed_public_result_preserves_unique_hold(self):
        _, candidate = self.redundant(); raw = candidate.read_bytes()
        self.env.update(LD_PRELOAD=self.hook, JASH_TEST_JOURNAL_GC_UNLINK_FAIL="1")
        failed = self.collect()
        self.env.pop("LD_PRELOAD"); self.env.pop("JASH_TEST_JOURNAL_GC_UNLINK_FAIL")
        item = failed["journal_gc"]["items"][0]; hold = candidate.parent / item["retained_as"]
        public = candidate.parent / item["public_ref"]; public.unlink()
        result = self.collect(); self.assertTrue(result["ok"])
        self.assertEqual(result["journal_gc"]["reclaimed_files"], 0); self.assertEqual(hold.read_bytes(), raw)
        self.assertEqual(result["journal_gc"]["items"][0]["action"], "retained_no_public_result")

    def test_inode_swap_restores_without_deleting_either_version(self):
        _, candidate = self.redundant(); raw = candidate.read_bytes()
        self.env.update(LD_PRELOAD=self.hook, JASH_TEST_JOURNAL_GC_SWAP="1")
        result = self.collect()
        self.env.pop("LD_PRELOAD"); self.env.pop("JASH_TEST_JOURNAL_GC_SWAP")
        self.assertFalse(result["ok"]); self.assertEqual(result["journal_gc"]["reclaimed_files"], 0)
        self.assertEqual(result["journal_gc"]["items"][0]["action"], "identity_changed")
        self.assertEqual(candidate.read_bytes(), b"raced")
        self.assertEqual((candidate.parent / ".saved-journal-original").read_bytes(), raw)

    def test_public_change_and_restore_collision_preserve_captured_copy(self):
        _, candidate = self.redundant(); raw = candidate.read_bytes()
        self.env.update(LD_PRELOAD=self.hook, JASH_TEST_JOURNAL_GC_PUBLIC_CHANGE="1")
        result = self.collect()
        self.env.pop("LD_PRELOAD"); self.env.pop("JASH_TEST_JOURNAL_GC_PUBLIC_CHANGE")
        self.assertFalse(result["ok"]); self.assertEqual(result["journal_gc"]["reclaimed_files"], 0)
        item = result["journal_gc"]["items"][0]
        self.assertEqual(item["action"], "public_or_content_changed")
        self.assertEqual((candidate.parent / item["retained_as"]).read_bytes(), raw)
        self.assertEqual(candidate.read_bytes(), b"occupied")

    def test_capture_sync_failure_keeps_hold_until_next_verified_pass(self):
        _, candidate = self.redundant(); raw = candidate.read_bytes()
        self.env.update(LD_PRELOAD=self.hook, JASH_TEST_JOURNAL_GC_SYNC_FAIL="1")
        result = self.collect()
        self.env.pop("LD_PRELOAD"); self.env.pop("JASH_TEST_JOURNAL_GC_SYNC_FAIL")
        self.assertFalse(result["ok"])
        item = result["journal_gc"]["items"][0]; self.assertEqual(item["action"], "capture_sync_failed")
        hold = candidate.parent / item["retained_as"]; self.assertEqual(hold.read_bytes(), raw)
        self.assertEqual(self.collect()["journal_gc"]["reclaimed_files"], 1); self.assertFalse(hold.exists())

    def test_gc_replay_never_removes_recreated_staging(self):
        _, candidate = self.redundant(); raw = candidate.read_bytes()
        request = {"id": "gc-once", "protocol_version": 2, "op": "store.journal.gc", "older_than_seconds": 0, "caps": ["store:manage"]}
        result, wire, reply = self.execute(request); self.assertTrue(result["ok"]); self.assertFalse(candidate.exists())
        candidate.write_bytes(raw); candidate.chmod(0o600)
        self.assertEqual(self.execute(request)[2], reply); self.assertEqual(candidate.read_bytes(), raw)
        req = self.work / "request"; req.write_bytes(wire)
        res = self.work / "response"; res.write_bytes(reply)
        receipt = candidate.parent / result["receipt_ref"]
        verified = subprocess.run([str(fixture.fixture.ROOT / "build/jef_verify"), "maintenance-receipt", self.runtime_key,
                                   str(req), str(res), str(receipt)], capture_output=True, timeout=10)
        self.assertEqual(verified.returncode, 0, verified.stderr)

    def test_pending_unique_and_different_bytes_are_never_collected(self):
        original, _, candidate = self.interrupted(); raw = candidate.read_bytes()
        result = self.collect(); self.assertTrue(result["ok"]); self.assertEqual(result["journal_gc"]["reclaimed_files"], 0)
        self.assertEqual(candidate.read_bytes(), raw); self.assertEqual(self.execute(original)[0]["error"], "maintenance_indeterminate")
        self.assertTrue(self.request("store.resolve", target_id=original["id"])["ok"])
        candidate.write_bytes(raw.replace(b'"applied":true', b'"applied":null'))
        result = self.collect(); self.assertEqual(result["journal_gc"]["reclaimed_files"], 0)
        self.assertTrue(candidate.exists()); self.assertEqual(result["journal_gc"]["items"][0]["action"], "retained_unverified")

    def test_age_and_unsafe_staging_are_retained(self):
        _, candidate = self.redundant()
        result = self.request("store.journal.gc")
        self.assertEqual(result["journal_gc"]["items"][0]["action"], "retained_age"); self.assertTrue(candidate.exists())
        candidate.chmod(0o644)
        self.assertEqual(self.collect()["journal_gc"]["items"][0]["action"], "retained_unsafe")
        candidate.unlink(); os.mkfifo(candidate)
        self.assertEqual(self.collect()["journal_gc"]["items"][0]["action"], "retained_unsafe")

    def test_conflicting_capture_name_never_overwrites_unique_hold(self):
        _, candidate = self.redundant(); raw = candidate.read_bytes()
        hold = candidate.parent / (".jgc-" + candidate.name[1:]); hold.write_bytes(b"unique-hold"); hold.chmod(0o600)
        result = self.collect()
        self.assertFalse(result["ok"]); self.assertEqual(result["journal_gc"]["reclaimed_files"], 0)
        self.assertEqual(candidate.read_bytes(), raw); self.assertEqual(hold.read_bytes(), b"unique-hold")
        item = next(entry for entry in result["journal_gc"]["items"] if entry["ref"] == candidate.name)
        self.assertEqual(item["action"], "capture_failed")

    def test_identical_bytes_without_valid_receipt_chain_are_retained(self):
        original, candidate = self.redundant(); raw = candidate.read_bytes()
        receipt = candidate.parent / json.loads(raw)["receipt_ref"]; receipt.write_bytes(b"{}")
        result = self.collect()
        self.assertTrue(result["ok"]); self.assertFalse(result["maintenance_complete"])
        self.assertEqual(result["journal_gc"]["reclaimed_files"], 0); self.assertEqual(candidate.read_bytes(), raw)
        self.assertEqual(result["journal_gc"]["items"][0]["action"], "retained_unverified")
        self.assertEqual(self.execute(original)[0]["error"], "maintenance_indeterminate")

    def test_recovery_pin_overrides_redundancy(self):
        _, candidate = self.redundant(); source = self.work / "pin-source"; source.write_bytes(candidate.read_bytes()); source.chmod(0o600)
        proc = subprocess.run([str(fixture.fixture.JASH), "-c", shlex.join(["cp", "--json", str(source), str(candidate)])],
                              cwd=self.work, env=dict(self.env, JASH_TIMELINE="1"), capture_output=True, timeout=20)
        self.assertEqual(proc.returncode, 0, proc.stderr); self.assertIsNotNone(json.loads(proc.stdout)["items"][0]["undo"])
        result = self.collect(); self.assertTrue(result["ok"])
        item = next(item for item in result["journal_gc"]["items"] if item["ref"] == candidate.name)
        self.assertEqual(item["action"], "retained_pin"); self.assertTrue(candidate.exists())

    def test_quota_pressure_does_not_require_erasing_unique_data(self):
        self.assertTrue(self.request("store.journal.quota", max_bytes=64 * 1024 * 1024)["ok"])
        _, candidate = self.redundant()
        observed = self.request("store.inspect"); filler = candidate.parent / ".quota-pressure"
        with filler.open("wb") as stream: stream.truncate(observed["journal_quota_bytes"] - observed["journal_used_bytes"] - 1)
        filler.chmod(0o600)
        result = self.collect(); self.assertTrue(result["ok"], result)
        self.assertEqual(result["journal_gc"]["quota_mode"], "redundant-gc-exempt")
        self.assertFalse(candidate.exists()); self.assertTrue(filler.exists())

    def test_incomplete_scan_does_not_remove_a_verified_prefix(self):
        _, candidate = self.redundant()
        for i in range(4097): (candidate.parent / f".limit-{i}").touch(mode=0o600)
        result = self.collect(); self.assertFalse(result["ok"])
        self.assertFalse(result["journal_gc"]["scan_complete"]); self.assertEqual(result["journal_gc"]["reclaimed_files"], 0)
        self.assertTrue(candidate.exists())

    def test_prepare_failure_or_closed_header_precedes_removal(self):
        _, candidate = self.redundant()
        self.env.update(LD_PRELOAD=self.hook, JASH_TEST_JOURNAL_ENOSPC="intent")
        result = self.collect()
        self.env.pop("LD_PRELOAD"); self.env.pop("JASH_TEST_JOURNAL_ENOSPC")
        self.assertEqual(result["error"], "maintenance_prepare_failed"); self.assertTrue(candidate.exists())
        request = {"id": "closed-header", "protocol_version": 2, "op": "store.journal.gc", "older_than_seconds": 0, "caps": ["store:manage"]}
        proc = subprocess.Popen([str(fixture.AGENT)], cwd=self.work, env=self.env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            json.loads(proc.stdout.readline()); proc.stdout.close(); proc.stdout = None
            proc.communicate((json.dumps(request) + "\n").encode(), timeout=10)
        finally:
            if proc.poll() is None: proc.kill(); proc.communicate(timeout=10)
        self.assertTrue(candidate.exists())

    def test_failed_terminal_after_collection_can_be_resolved_without_recollecting(self):
        _, candidate = self.redundant(); raw = candidate.read_bytes()
        request = {"id": "gc-commit-failed", "protocol_version": 2, "op": "store.journal.gc", "older_than_seconds": 0, "caps": ["store:manage"]}
        self.env.update(LD_PRELOAD=self.hook, JASH_TEST_MAINT_COMMIT_FAIL="1")
        failed = self.execute(request)[0]
        self.env.pop("LD_PRELOAD"); self.env.pop("JASH_TEST_MAINT_COMMIT_FAIL")
        self.assertEqual(failed["transaction"], "indeterminate"); self.assertTrue(failed["applied"]); self.assertFalse(candidate.exists())
        candidate.write_bytes(raw); candidate.chmod(0o600)
        self.assertTrue(self.request("store.resolve", target_id=request["id"])["ok"])
        replay = self.execute(request)[0]
        self.assertEqual(replay["journal_gc"]["reclaimed_files"], 1); self.assertTrue(candidate.exists())

    def test_capability_and_peer_guards_remain_mandatory(self):
        _, candidate = self.redundant()
        request = {"id": "unauthorized", "protocol_version": 2, "op": "store.journal.gc", "older_than_seconds": 0, "caps": ["store:read"]}
        self.assertEqual(self.execute(request)[0]["error"], "capability denied"); self.assertTrue(candidate.exists())
        peer = subprocess.Popen([str(fixture.AGENT)], cwd=self.work, env=self.env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            json.loads(peer.stdout.readline())
            self.assertEqual(self.collect()["error"], "store_busy"); self.assertTrue(candidate.exists())
        finally:
            peer.stdin.close(); peer.stdin = None; peer.communicate(timeout=10)


class JournalGCCore(unittest.TestCase):
    def test_redundancy_capture_and_public_evidence_under_sanitizers(self):
        with tempfile.TemporaryDirectory(prefix="jash-journal-gc-core-") as temp:
            binary = str(Path(temp) / "test")
            flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie"] if os.environ.get("SANITIZE") else []
            sources = ["tests/test_journal_gc_core.c", "c/core/agent_json.c", "c/core/json_writer.c", "c/core/recovery.c", "c/module/blake3.c",
                       "c/core/execution_ir.c", "c/core/agent_cgroup.c", "c/module/jash_crypto.c", "c/osif/posix_common.c", "libjef/jef_verify.c", "libjef/ed25519.c"]
            command = ["cc", "-std=gnu11", "-g", "-O1", "-ffunction-sections", "-fdata-sections", *flags]
            for include in ("c", "c/sys", "libjef"): command += ["-I", str(fixture.fixture.ROOT / include)]
            command += [str(fixture.fixture.ROOT / source) for source in sources] + ["-Wl,--gc-sections", "-o", binary]
            compiled = subprocess.run(command, capture_output=True, timeout=180)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            checked = subprocess.run([binary, temp], capture_output=True, timeout=10)
            self.assertEqual(checked.returncode, 0, checked.stderr)


class JournalQuota(unittest.TestCase):
    setUp = fixture.Store.setUp
    setUpClass = classmethod(fixture.Store.setUpClass.__func__)
    execute = Transactions.execute
    request = fixture.Store.request
    interrupted = Resolution.interrupted

    def test_quota_admission_precedes_intent_and_preserves_retry_id(self):
        configured = self.request("store.journal.quota", max_bytes=64 * 1024 * 1024)
        self.assertTrue(configured.get("ok"), configured)
        self.assertEqual(configured["journal_quota_bytes"], 64 * 1024 * 1024)
        before = self.request("store.inspect")
        self.assertGreater(before["journal_required_headroom_bytes"], 16 * 1024 * 1024)
        limit = before["journal_used_bytes"] + before["journal_required_headroom_bytes"] + 16384
        self.assertTrue(self.request("store.journal.quota", max_bytes=limit)["ok"])
        directory = self.home / ".jash/agent_maintenance"
        filler = directory / ".quota-pressure"
        used = sum(p.stat().st_size for p in directory.iterdir())
        with filler.open("wb") as stream: stream.truncate(limit - used - 1)
        filler.chmod(0o600)
        original = {"id": "retry-pressure", "protocol_version": 2, "op": "store.quota", "max_bytes": 123456, "caps": ["store:manage"]}
        evidence = {p.name: p.stat().st_size for p in directory.iterdir()}
        refused = self.execute(original)[0]
        self.assertEqual(refused["error"], "journal_quota_exceeded"); self.assertEqual(refused["transaction"], "not_started")
        self.assertFalse(refused["applied"]); self.assertFalse((self.home / ".jash/agent_store/quota").exists())
        self.assertEqual(evidence, {p.name: p.stat().st_size for p in directory.iterdir()})
        self.assertTrue(self.request("store.journal.quota", max_bytes=0)["ok"])
        self.assertTrue(filler.exists())
        self.assertTrue(self.execute(original)[0]["ok"])

    def configure(self, limit=64 * 1024 * 1024):
        result = self.request("store.journal.quota", max_bytes=limit)
        self.assertTrue(result["ok"], result)
        return result

    def pressure(self, remaining=1):
        observed = self.request("store.inspect")
        path = self.home / ".jash/agent_maintenance/.quota-pressure"
        with path.open("wb") as stream: stream.truncate(observed["journal_quota_bytes"] - observed["journal_used_bytes"] - remaining)
        path.chmod(0o600)
        return path

    def test_small_quota_rejected_before_preparation_and_preview_preserves_policy(self):
        original = {"id": "small", "protocol_version": 2, "op": "store.journal.quota", "max_bytes": 1, "caps": ["store:manage"]}
        refused = self.execute(original)[0]
        self.assertEqual(refused["error"], "journal_quota_exceeded"); self.assertEqual(refused["transaction"], "not_started")
        self.assertFalse((self.home / ".jash/agent_maintenance").exists())
        self.assertFalse((self.home / ".jash/agent_store/journal_quota").exists())
        preview = self.request("store.journal.quota", max_bytes=64 * 1024 * 1024, dry_run=True)
        self.assertTrue(preview["ok"]); self.assertFalse(preview["applied"])
        self.assertEqual(preview["journal_quota_bytes"], 0)
        self.assertFalse((self.home / ".jash/agent_store/journal_quota").exists())

    def test_measurement_counts_all_regular_staging_and_matches_actual_bytes(self):
        self.configure(); directory = self.home / ".jash/agent_maintenance"
        extra = directory / ".unattributed-staging"; extra.write_bytes(b"extra"); extra.chmod(0o600)
        view = self.request("store.inspect")
        self.assertTrue(view["journal_measurement_complete"])
        self.assertEqual(view["journal_used_bytes"], sum(p.stat().st_size for p in directory.iterdir()))
        self.assertEqual(view["journal_entries"], len(list(directory.iterdir())))
        link = directory / ".ambiguous-link"; link.symlink_to(extra)
        view = self.request("store.inspect")
        self.assertFalse(view["journal_measurement_complete"]); self.assertIsNone(view["journal_used_bytes"])
        refused = self.request("store.quota", max_bytes=123456)
        self.assertEqual(refused["error"], "journal_measurement_incomplete"); self.assertFalse(refused["applied"])
        self.assertTrue(link.is_symlink())

    def test_space_and_inode_preflight_fail_without_consuming_id(self):
        self.configure(); directory = self.home / ".jash/agent_maintenance"
        before = {p.name: p.read_bytes() for p in directory.iterdir()}
        original = {"id": "space", "protocol_version": 2, "op": "store.quota", "max_bytes": 123456, "caps": ["store:manage"]}
        for mode in ("bytes", "inodes", "unknown"):
            with self.subTest(mode=mode):
                self.env.update(LD_PRELOAD=self.hook, JASH_TEST_JOURNAL_SPACE=mode)
                refused = self.execute(original)[0]
                self.assertEqual(refused["error"], "journal_space_unknown" if mode == "unknown" else "journal_space_insufficient")
                self.assertEqual(refused["transaction"], "not_started"); self.assertFalse(refused["applied"])
                self.assertEqual(before, {p.name: p.read_bytes() for p in directory.iterdir()})
        self.env.pop("LD_PRELOAD"); self.env.pop("JASH_TEST_JOURNAL_SPACE")
        self.assertTrue(self.execute(original)[0]["ok"])

    def test_injected_enospc_before_intent_prevents_effect_and_allows_retry(self):
        self.configure()
        original = {"id": "enospc-before", "protocol_version": 2, "op": "store.quota", "max_bytes": 123456, "caps": ["store:manage"]}
        self.env.update(LD_PRELOAD=self.hook, JASH_TEST_JOURNAL_ENOSPC="intent")
        failed = self.execute(original)[0]
        self.assertEqual(failed["error"], "maintenance_prepare_failed"); self.assertFalse(failed["applied"])
        self.assertFalse((self.home / ".jash/agent_store/quota").exists())
        self.env.pop("LD_PRELOAD"); self.env.pop("JASH_TEST_JOURNAL_ENOSPC")
        self.assertTrue(self.execute(original)[0]["ok"])

    def test_enospc_after_effect_retains_indeterminate_evidence(self):
        self.configure()
        original = {"id": "enospc-after", "protocol_version": 2, "op": "store.quota", "max_bytes": 123456, "caps": ["store:manage"]}
        self.env.update(LD_PRELOAD=self.hook, JASH_TEST_JOURNAL_ENOSPC="result")
        failed = self.execute(original)[0]
        self.assertEqual(failed["transaction"], "indeterminate"); self.assertTrue(failed["applied"])
        self.env.pop("LD_PRELOAD"); self.env.pop("JASH_TEST_JOURNAL_ENOSPC")
        self.assertEqual((self.home / ".jash/agent_store/quota").read_text(), "123456\n")
        directory = self.home / ".jash/agent_maintenance"
        before = {p.name: p.read_bytes() for p in directory.iterdir()}
        self.assertEqual(self.execute(original)[0]["error"], "maintenance_indeterminate")
        self.assertFalse(self.request("store.resolve", target_id=original["id"])["ok"])
        self.assertEqual(before, {p.name: p.read_bytes() for p in directory.iterdir()})

    def test_recovery_is_explicitly_quota_exempt_and_keeps_pressure_file(self):
        self.configure()
        original, _, candidate = self.interrupted()
        raw = candidate.read_bytes(); filler = self.pressure()
        resolved = self.request("store.resolve", target_id=original["id"])
        self.assertTrue(resolved["ok"]); self.assertEqual(resolved["journal_quota_mode"], "recovery-exempt")
        self.assertEqual(self.execute(original)[2], raw); self.assertTrue(filler.exists())
        view = self.request("store.inspect")
        self.assertGreater(view["journal_used_bytes"], view["journal_quota_bytes"])
        self.assertEqual(self.request("store.quota", max_bytes=999999)["error"], "journal_quota_exceeded")

    def test_preparation_closure_remains_available_under_journal_pressure(self):
        self.configure()
        original = {"id": "close-under-pressure", "protocol_version": 2, "op": "store.quota", "max_bytes": 123456, "caps": ["store:manage"]}
        self.env.update(LD_PRELOAD=self.hook, JASH_TEST_MAINT_ADMISSION_FAIL="1")
        self.assertFalse(self.execute(original)[0]["applied"])
        self.env.pop("LD_PRELOAD"); self.env.pop("JASH_TEST_MAINT_ADMISSION_FAIL")
        filler = self.pressure()
        resolved = self.request("store.resolve", target_id=original["id"])
        self.assertEqual(resolved["status"], "closed"); self.assertTrue(filler.exists())
        self.assertEqual(self.execute(original)[0]["error"], "effects_not_admitted")
        self.assertFalse((self.home / ".jash/agent_store/quota").exists())

    def test_policy_replay_is_signed_and_does_not_reapply_old_limit(self):
        original = {"id": "journal-policy", "protocol_version": 2, "op": "store.journal.quota", "max_bytes": 64 * 1024 * 1024, "caps": ["store:manage"]}
        first, wire, raw = self.execute(original)
        self.configure(128 * 1024 * 1024)
        self.assertEqual(self.execute(original)[2], raw)
        self.assertEqual(self.request("store.inspect")["journal_quota_bytes"], 128 * 1024 * 1024)
        req = self.work / "request"; req.write_bytes(wire)
        res = self.work / "response"; res.write_bytes(raw)
        receipt = self.home / ".jash/agent_maintenance" / first["receipt_ref"]
        verified = subprocess.run([str(fixture.fixture.ROOT / "build/jef_verify"), "maintenance-receipt", self.runtime_key,
                                   str(req), str(res), str(receipt)], capture_output=True, timeout=10)
        self.assertEqual(verified.returncode, 0, verified.stderr)

    def test_policy_publication_failure_keeps_previous_limit_and_replays_failure(self):
        self.configure()
        self.env.update(LD_PRELOAD=self.hook, JASH_TEST_JOURNAL_POLICY_FAIL="1")
        original = {"id": "policy-failed", "protocol_version": 2, "op": "store.journal.quota", "max_bytes": 128 * 1024 * 1024, "caps": ["store:manage"]}
        failed, _, raw = self.execute(original)
        self.env.pop("LD_PRELOAD"); self.env.pop("JASH_TEST_JOURNAL_POLICY_FAIL")
        self.assertEqual(failed["error"], "quota_publish_failed"); self.assertFalse(failed["applied"])
        self.assertEqual(self.request("store.inspect")["journal_quota_bytes"], 64 * 1024 * 1024)
        self.assertEqual(self.execute(original)[2], raw)

    def test_invalid_policy_is_never_interpreted_as_unlimited(self):
        self.configure(); path = self.home / ".jash/agent_store/journal_quota"
        path.write_bytes(b"garbage\n")
        refused = self.request("store.quota", max_bytes=123456)
        self.assertEqual(refused["error"], "journal_policy_invalid"); self.assertIsNone(refused["journal_quota_bytes"])
        self.assertFalse((self.home / ".jash/agent_store/quota").exists())

    def test_peer_is_excluded_before_policy_change_or_admission(self):
        self.configure()
        peer = subprocess.Popen([str(fixture.AGENT)], cwd=self.work, env=self.env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            json.loads(peer.stdout.readline())
            self.assertEqual(self.request("store.journal.quota", max_bytes=0)["error"], "store_busy")
            self.assertEqual(self.request("store.quota", max_bytes=123456)["error"], "store_busy")
            self.assertEqual(self.request("store.inspect")["journal_quota_bytes"], 64 * 1024 * 1024)
        finally:
            peer.stdin.close(); peer.stdin = None; peer.communicate(timeout=10)

    def test_concurrent_admission_remeasures_usage_under_exclusive_lease(self):
        self.configure()
        required = self.request("store.inspect")["journal_required_headroom_bytes"]
        self.pressure(remaining=required + 512)
        ready = threading.Barrier(2)
        def attempt(value):
            request = {"id": f"concurrent-{value}", "protocol_version": 2, "op": "store.quota", "max_bytes": value, "caps": ["store:manage"]}
            ready.wait(timeout=10)
            for _ in range(40):
                execution = subprocess.run([str(fixture.AGENT)], cwd=self.work, env=self.env,
                                           input=(json.dumps(request) + "\n").encode(), capture_output=True, timeout=20)
                self.assertEqual(execution.returncode, 0, execution.stderr)
                result = json.loads(execution.stdout.splitlines()[-1])
                if result.get("error") != "store_busy": return result
                time.sleep(.01)
            self.fail("contenders never left store_busy")
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
            results = list(pool.map(attempt, (123456, 654321)))
        self.assertEqual(sum(result["ok"] for result in results), 1, results)
        self.assertEqual([result["error"] for result in results if not result["ok"]], ["journal_quota_exceeded"])
        winner = next(result for result in results if result["ok"])
        self.assertEqual(self.request("store.inspect")["quota_bytes"], int(winner["id"].split("-")[1]))

    def test_admission_checks_namespace_slots_before_effects(self):
        self.configure(); directory = self.home / ".jash/agent_maintenance"
        for i in range(4091 - len(list(directory.iterdir()))): (directory / f".slot-{i}").touch(mode=0o600)
        refused = self.request("store.quota", max_bytes=123456)
        self.assertEqual(refused["error"], "journal_entry_limit"); self.assertEqual(refused["transaction"], "not_started")
        self.assertFalse((self.home / ".jash/agent_store/quota").exists())


class JournalQuotaCore(unittest.TestCase):
    def test_measurement_headroom_and_policy_boundaries_under_sanitizers(self):
        with tempfile.TemporaryDirectory(prefix="jash-journal-quota-core-") as temp:
            binary = str(Path(temp) / "test")
            flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie"] if os.environ.get("SANITIZE") else []
            sources = ["tests/test_journal_quota_core.c", "c/core/json_writer.c", "c/core/recovery.c", "c/module/blake3.c",
                       "c/module/jash_crypto.c", "c/osif/posix_common.c", "libjef/jef_verify.c", "libjef/ed25519.c"]
            command = ["cc", "-std=gnu11", "-g", "-O1", "-ffunction-sections", "-fdata-sections", *flags]
            for include in ("c", "c/sys", "libjef"): command += ["-I", str(fixture.fixture.ROOT / include)]
            command += [str(fixture.fixture.ROOT / source) for source in sources] + ["-Wl,--gc-sections", "-o", binary]
            compiled = subprocess.run(command, capture_output=True, timeout=180)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            checked = subprocess.run([binary, temp], capture_output=True, timeout=10)
            self.assertEqual(checked.returncode, 0, checked.stderr)


class History(unittest.TestCase):
    setUp = fixture.Store.setUp
    setUpClass = classmethod(fixture.Store.setUpClass.__func__)
    execute = Transactions.execute
    request = fixture.Store.request
    interrupted = Resolution.interrupted

    def test_pages_discover_ids_and_resume_without_duplicate_records(self):
        ids = []
        for value in (100000, 200000, 300000): ids.append(self.request("store.quota", max_bytes=value)["id"])
        first = self.request("store.history", limit=1)
        self.assertEqual(first.get("schema"), "jash.agent-maintenance-history.v1", first)
        self.assertTrue(first["ok"]); self.assertEqual(first["total_records"], 3)
        rows = first["items"]; page = first
        while page["next_cursor"]:
            page = self.request("store.history", limit=1, cursor=page["next_cursor"])
            self.assertEqual(page["snapshot"], first["snapshot"])
            rows += page["items"]
        self.assertEqual({row["target_id"] for row in rows}, set(ids)); self.assertEqual(len(rows), 3)
        self.assertEqual([row["record_key"] for row in rows], sorted(row["record_key"] for row in rows))
        self.assertTrue(all(row["state"] == "committed" for row in rows))
        self.assertTrue(all(row["effect_admission"] == "admitted" for row in rows))

    def two(self):
        for value in (100000, 200000): self.request("store.quota", max_bytes=value)
        return self.request("store.history", limit=1)

    def unchanged(self, directory):
        return {p.name: (p.read_bytes(), p.stat().st_ino, p.stat().st_mtime_ns) for p in directory.iterdir() if p.is_file()}

    def test_read_only_pages_do_not_create_or_change_journal(self):
        empty = self.request("store.history")
        self.assertTrue(empty["ok"]); self.assertEqual(empty["items"], []); self.assertEqual(empty["total_records"], 0)
        self.assertFalse((self.home / ".jash/agent_maintenance").exists())
        first = self.two(); directory = self.home / ".jash/agent_maintenance"; before = self.unchanged(directory)
        self.request("store.history", cursor=first["next_cursor"])
        self.assertEqual(self.request("store.history", limit=1)["snapshot"], first["snapshot"])
        self.assertEqual(before, self.unchanged(directory))

    def test_cursor_rejects_changes_before_the_next_offset(self):
        first = self.two()
        self.assertTrue(self.request("store.quota", max_bytes=300000)["ok"])
        rejected = self.request("store.history", cursor=first["next_cursor"])
        self.assertEqual(rejected["error"], "cursor_stale"); self.assertEqual(rejected["items"], [])
        self.assertIsNone(rejected["snapshot"]); self.assertIsNone(rejected["next_cursor"])

    def test_same_length_edit_with_restored_mtime_invalidates_cursor(self):
        first = self.two(); directory = self.home / ".jash/agent_maintenance"
        intent = next(directory.glob("*.intent")); st = intent.stat(); raw = intent.read_bytes()
        intent.write_bytes(raw.replace(b"100000", b"100001").replace(b"200000", b"200001"))
        os.utime(intent, ns=(st.st_atime_ns, st.st_mtime_ns))
        refused = self.request("store.history", cursor=first["next_cursor"])
        self.assertEqual(refused["error"], "cursor_stale"); self.assertEqual(refused["items"], [])

    def test_replaced_identical_inode_invalidates_cursor(self):
        first = self.two(); intent = next((self.home / ".jash/agent_maintenance").glob("*.intent"))
        raw = intent.read_bytes(); st = intent.stat(); copy = self.work / "replacement"
        copy.write_bytes(raw); copy.chmod(0o600); os.utime(copy, ns=(st.st_atime_ns, st.st_mtime_ns)); copy.replace(intent)
        self.assertEqual(self.request("store.history", cursor=first["next_cursor"])["error"], "cursor_stale")

    def test_unattributed_special_files_remain_visible_without_opening_them(self):
        self.two(); directory = self.home / ".jash/agent_maintenance"
        fifo = directory / "unknown-pipe"; os.mkfifo(fifo)
        link = directory / "unknown-link"; link.symlink_to(self.work / "absent")
        strange = os.fsencode(directory) + b"/unknown-\xff"
        fd = os.open(strange, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600); os.write(fd, b"evidence"); os.close(fd)
        page = self.request("store.history", limit=64)
        unknown = [row for row in page["items"] if row["state"] == "unattributed"]
        self.assertEqual(len(unknown), 3); self.assertTrue(all(row["target_id"] is None for row in unknown))
        self.assertTrue(any(isinstance(row["example_ref"], dict) for row in unknown))
        self.assertTrue(fifo.exists()); self.assertTrue(link.is_symlink())

    def test_orphan_signed_receipts_discover_id_but_do_not_resolve_it(self):
        original, _, candidate = self.interrupted()
        next(candidate.parent.glob("*.intent")).unlink()
        candidate.unlink()
        page = self.request("store.history")
        self.assertEqual(len(page["items"]), 1); row = page["items"][0]
        self.assertEqual(row["target_id"], original["id"]); self.assertEqual(row["id_source"], "signed_receipt")
        self.assertEqual(row["effect_admission"], "intent_missing"); self.assertEqual(row["state"], "unresolved")
        self.assertFalse(self.request("store.resolve", target_id=original["id"])["ok"])
        self.assertEqual(self.request("store.gc", older_than_seconds=0)["error"], "maintenance_incomplete")

    def test_discovered_pending_id_uses_existing_inspection_and_resolution(self):
        original, _, candidate = self.interrupted(identifier='"' * 47)
        page = self.request("store.history"); self.assertEqual(page["total_records"], 1)
        row = page["items"][0]; self.assertEqual(row["state"], "unresolved")
        self.assertEqual(row["target_id"], original["id"]); self.assertEqual(row["staging_files"], 1)
        view = self.request("store.inspect", target_id=row["target_id"])
        self.assertEqual(view["state"], "recoverable")
        before = self.unchanged(candidate.parent)
        self.assertEqual(self.request("store.history")["snapshot"], page["snapshot"])
        self.assertEqual(before, self.unchanged(candidate.parent))
        self.assertTrue(self.request("store.resolve", target_id=row["target_id"])["ok"])
        self.assertEqual(self.request("store.history")["items"][0]["state"], "committed")

    def test_catalog_limit_returns_no_prefix_or_cursor(self):
        self.two(); directory = self.home / ".jash/agent_maintenance"
        for i in range(4097): (directory / f".limit-{i}").touch(mode=0o600)
        refused = self.request("store.history", limit=1)
        self.assertEqual(refused["error"], "scan_limit"); self.assertEqual(refused["entries_scanned"], 4096)
        self.assertFalse(refused["scan_complete"]); self.assertEqual(refused["items"], [])
        self.assertIsNone(refused["total_records"]); self.assertIsNone(refused["next_cursor"])

    def test_read_budget_and_verification_budget_are_explicit(self):
        original, _, candidate = self.interrupted()
        # This is invalid staging but still part of a discoverable transaction.
        with candidate.open("wb") as stream: stream.truncate(9 * 1024 * 1024)
        page = self.request("store.history")
        self.assertTrue(page["ok"]); self.assertEqual(page["items"][0]["verification"], "not_checked")
        self.assertEqual(page["items"][0]["reason"], "verification_limit")
        with candidate.open("wb") as stream: stream.truncate(16 * 1024 * 1024 + 1)
        refused = self.request("store.history")
        self.assertEqual(refused["error"], "read_limit"); self.assertEqual(refused["items"], [])
        self.assertFalse(self.request("store.resolve", target_id=original["id"])["ok"])

    def test_cumulative_read_limit_never_returns_a_partial_prefix(self):
        self.two(); directory = self.home / ".jash/agent_maintenance"
        for i in range(5):
            path = directory / f"oversized-catalog-{i}"
            with path.open("wb") as stream: stream.truncate(14 * 1024 * 1024)
            path.chmod(0o600)
        refused = self.request("store.history", limit=1)
        self.assertEqual(refused["error"], "read_limit"); self.assertEqual(refused["items"], [])
        self.assertLessEqual(refused["bytes_read"], 64 * 1024 * 1024)
        self.assertIsNone(refused["snapshot"]); self.assertIsNone(refused["next_cursor"])

    def test_cursor_namespace_and_key_identity_are_bound(self):
        first = self.two(); old_home = self.home; directory = self.home / ".jash/agent_maintenance"
        other = self.work / "other-home"; other.mkdir()
        shutil.copytree(old_home / ".jash", other / ".jash")
        old_env = self.env; self.env = dict(self.env, HOME=str(other))
        try:
            self.assertEqual(self.request("store.history", cursor=first["next_cursor"])["error"], "cursor_stale")
        finally: self.env = old_env
        key = self.home / ".jash/keys/ed25519.seed"; key.write_bytes(b"\x01" * 32)
        self.assertEqual(self.request("store.history", cursor=first["next_cursor"])["error"], "cursor_stale")
        page = self.request("store.history", limit=64)
        self.assertTrue(any(row["state"] == "unattributed" for row in page["items"]))
        self.assertFalse(any(row["state"] == "committed" for row in page["items"]))

    def test_invalid_pagination_fields_and_authority_are_rejected(self):
        first = self.two(); directory = self.home / ".jash/agent_maintenance"; before = self.unchanged(directory)
        request = {"id": "history", "protocol_version": 2, "op": "store.history", "caps": ["store:read"]}
        for invalid in (dict(request, limit=0), dict(request, limit=65), dict(request, cursor=""), dict(request, cursor="m1:x:1"),
                        dict(request, cursor="m1:" + "0" * 64 + ":01"), dict(request, target_id="x"), dict(request, caps=[])):
            self.assertIn("error", self.execute(invalid)[0])
        out_of_range = "m1:" + first["snapshot"] + ":4096"
        self.assertEqual(self.request("store.history", cursor=out_of_range)["error"], "cursor_range")
        self.assertEqual(before, self.unchanged(directory))

    def test_active_peer_does_not_turn_history_into_mutation_authority(self):
        original, _, _ = self.interrupted()
        peer = subprocess.Popen([str(fixture.AGENT)], cwd=self.work, env=self.env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            json.loads(peer.stdout.readline())
            page = self.execute({"id": "reader", "protocol_version": 2, "op": "store.history", "caps": ["store:read"]})[0]
            self.assertTrue(page["ok"]); self.assertEqual(page["authority"], "observation-only")
            self.assertEqual(self.request("store.resolve", target_id=original["id"])["error"], "store_busy")
        finally:
            peer.stdin.close(); peer.stdin = None; peer.communicate(timeout=10)

    def test_change_during_scan_does_not_return_a_page(self):
        self.two(); directory = self.home / ".jash/agent_maintenance"
        marker = self.work / "history-scan"
        proc = subprocess.Popen([str(fixture.AGENT)], cwd=self.work, env=dict(self.env, LD_PRELOAD=self.hook, JASH_TEST_HISTORY_STAT_GATE=str(marker)),
                                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            json.loads(proc.stdout.readline())
            request = {"id": "raced", "protocol_version": 2, "op": "store.history", "caps": ["store:read"]}
            proc.stdin.write((json.dumps(request) + "\n").encode()); proc.stdin.flush()
            deadline = time.monotonic() + 10
            while not marker.exists() and proc.poll() is None and time.monotonic() < deadline: time.sleep(.01)
            self.assertTrue(marker.exists())
            (directory / "raced-evidence").write_bytes(b"unique")
            Path(str(marker) + ".release").touch()
            proc.stdin.close(); proc.stdin = None
            stdout, stderr = proc.communicate(timeout=20); self.assertEqual(proc.returncode, 0, stderr)
            page = json.loads(stdout.splitlines()[-1]); self.assertEqual(page["error"], "journal_changed")
            self.assertEqual(page["items"], []); self.assertIsNone(page["next_cursor"])
            self.assertEqual((directory / "raced-evidence").read_bytes(), b"unique")
        finally:
            if proc.poll() is None: proc.kill(); proc.communicate(timeout=10)


class HistoryCore(unittest.TestCase):
    def test_bounded_snapshot_cursor_and_metadata_under_sanitizers(self):
        with tempfile.TemporaryDirectory(prefix="jash-history-core-") as temp:
            binary = str(Path(temp) / "test")
            flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie"] if os.environ.get("SANITIZE") else []
            sources = ["tests/test_maintenance_history_core.c", "c/core/agent_json.c", "c/core/json_writer.c", "c/core/recovery.c", "c/module/blake3.c",
                       "c/module/jash_crypto.c", "c/osif/posix_common.c", "libjef/jef_verify.c", "libjef/ed25519.c"]
            command = ["cc", "-std=gnu11", "-g", "-O1", "-ffunction-sections", "-fdata-sections", *flags]
            for include in ("c", "c/sys", "libjef"): command += ["-I", str(fixture.fixture.ROOT / include)]
            command += [str(fixture.fixture.ROOT / source) for source in sources] + ["-Wl,--gc-sections", "-o", binary]
            compiled = subprocess.run(command, capture_output=True, timeout=180)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            checked = subprocess.run([binary, temp], capture_output=True, timeout=10)
            self.assertEqual(checked.returncode, 0, checked.stderr)
            document = json.loads((Path(temp) / "history.json").read_bytes())
            self.assertTrue(document["ok"]); self.assertEqual(len(document["items"]), 2)
            self.assertEqual(document["total_records"], 4); self.assertIsNotNone(document["next_cursor"])


class Admission(unittest.TestCase):
    setUp = fixture.Store.setUp
    setUpClass = classmethod(fixture.Store.setUpClass.__func__)
    execute = Transactions.execute
    request = fixture.Store.request
    kill_at = Resolution.kill_at
    inspect = lambda self, target: self.request("store.inspect", target_id=target)

    def original(self, op="store.quota", **fields):
        value = {"id": "admit", "protocol_version": 2, "op": op, "caps": ["store:manage"], **fields}
        if op == "store.quota": value.setdefault("max_bytes", 123456)
        return value

    def prepared(self):
        original = self.original()
        self.kill_at(original, "JASH_TEST_MAINT_ADMISSION_GATE")
        return original

    def test_failed_admission_prevents_effect_and_can_be_closed(self):
        original = {"id": "admission-failure", "protocol_version": 2, "op": "store.quota", "max_bytes": 123456, "caps": ["store:manage"]}
        self.env.update(LD_PRELOAD=self.hook, JASH_TEST_MAINT_ADMISSION_FAIL="1")
        failed, wire, _ = self.execute(original)
        self.assertFalse(failed["applied"], failed)
        self.assertFalse((self.home / ".jash/agent_store/quota").exists())
        self.env.pop("LD_PRELOAD"); self.env.pop("JASH_TEST_MAINT_ADMISSION_FAIL")
        preview = self.request("store.resolve", target_id=original["id"], dry_run=True)
        self.assertEqual(preview["status"], "closable"); self.assertFalse(preview["published"])
        closed = self.request("store.resolve", target_id=original["id"])
        self.assertEqual(closed["status"], "closed"); self.assertTrue(closed["maintenance_complete"])
        result, _, raw = self.execute(original)
        self.assertEqual(result["effect_admission"], "closed"); self.assertFalse(result["applied"])
        self.assertEqual(result["error"], "effects_not_admitted")
        self.assertFalse((self.home / ".jash/agent_store/quota").exists())
        self.assertTrue(self.request("store.quota", max_bytes=999999)["ok"])
        self.assertEqual(self.execute(original)[2], raw)
        req = self.work / "request"; req.write_bytes(wire)
        res = self.work / "response"; res.write_bytes(raw)
        receipt = self.home / ".jash/agent_maintenance" / result["receipt_ref"]
        verified = subprocess.run([str(fixture.fixture.ROOT / "build/jef_verify"), "maintenance-receipt", self.runtime_key,
                                   str(req), str(res), str(receipt)], capture_output=True, timeout=10)
        self.assertEqual(verified.returncode, 0, verified.stderr)

    def test_killed_before_admission_can_close_but_not_repeat_original(self):
        original = self.prepared()
        view = self.inspect(original["id"])
        self.assertEqual(view["effect_protocol"], "admission-v1"); self.assertEqual(view["effect_admission"], "decision_missing")
        self.assertEqual(view["state"], "closable"); self.assertTrue(view["resolution_eligible"])
        self.assertFalse((self.home / ".jash/agent_store/quota").exists())
        self.assertEqual(self.execute(original)[0]["error"], "maintenance_indeterminate")
        self.assertEqual(self.request("store.resolve", target_id=original["id"])["status"], "closed")
        self.assertEqual(self.execute(original)[0]["error"], "effects_not_admitted")
        self.assertFalse((self.home / ".jash/agent_store/quota").exists())

    def test_killed_after_admission_before_effect_stays_indeterminate(self):
        original = self.original()
        self.kill_at(original, "JASH_TEST_MAINT_ADMISSION_AFTER_GATE")
        self.assertFalse((self.home / ".jash/agent_store/quota").exists())
        view = self.inspect(original["id"])
        self.assertEqual(view["effect_admission"], "admitted"); self.assertEqual(view["state"], "unresolved")
        self.assertEqual(self.request("store.resolve", target_id=original["id"])["error"], "terminal_unavailable")
        self.assertEqual(self.execute(original)[0]["error"], "maintenance_indeterminate")

    def test_admission_directory_sync_failure_prevents_effect_without_false_closure(self):
        original = self.original()
        self.env.update(LD_PRELOAD=self.hook, JASH_TEST_MAINT_ADMISSION_SYNC_FAIL="1")
        failed = self.execute(original)[0]
        self.env.pop("LD_PRELOAD"); self.env.pop("JASH_TEST_MAINT_ADMISSION_SYNC_FAIL")
        self.assertFalse(failed["applied"]); self.assertFalse((self.home / ".jash/agent_store/quota").exists())
        self.assertEqual(self.inspect(original["id"])["effect_admission"], "admitted")
        self.assertFalse(self.request("store.resolve", target_id=original["id"])["ok"])

    def test_closure_decision_survives_kill_before_terminal(self):
        original = self.prepared()
        resolve = {"id": "close", "protocol_version": 2, "op": "store.resolve", "target_id": original["id"], "caps": ["store:manage"]}
        self.kill_at(resolve, "JASH_TEST_MAINT_ADMISSION_AFTER_GATE")
        view = self.inspect(original["id"])
        self.assertEqual(view["effect_admission"], "closed"); self.assertEqual(view["state"], "closable")
        self.assertEqual(self.execute(original)[0]["error"], "maintenance_indeterminate")
        self.assertEqual(self.request("store.resolve", target_id=original["id"])["status"], "closed")
        self.assertEqual(self.execute(original)[0]["error"], "effects_not_admitted")

    def test_closure_result_publication_failure_resumes_original_evidence(self):
        original = self.prepared()
        self.env.update(LD_PRELOAD=self.hook, JASH_TEST_MAINT_COMMIT_FAIL="1")
        failed = self.request("store.resolve", target_id=original["id"])
        self.env.pop("LD_PRELOAD"); self.env.pop("JASH_TEST_MAINT_COMMIT_FAIL")
        self.assertEqual(failed["error"], "closure_unconfirmed")
        candidate = next((self.home / ".jash/agent_maintenance").glob(".*.result.*.tmp")); raw = candidate.read_bytes()
        self.assertEqual(self.request("store.resolve", target_id=original["id"])["status"], "recovered")
        self.assertEqual(self.execute(original)[2], raw)

    def test_invalid_decision_blocks_closure_and_terminal_replay(self):
        original = self.prepared()
        directory = self.home / ".jash/agent_maintenance"
        ref = self.inspect(original["id"])["decision_ref"]; decision = directory / ref
        decision.write_bytes(b"{}"); decision.chmod(0o600)
        before = decision.read_bytes()
        self.assertEqual(self.inspect(original["id"])["effect_admission"], "decision_invalid")
        self.assertFalse(self.request("store.resolve", target_id=original["id"])["ok"])
        self.assertEqual(decision.read_bytes(), before)
        decision.unlink()
        self.assertTrue(self.request("store.resolve", target_id=original["id"])["ok"])
        decision.write_bytes(b"{}"); self.assertEqual(self.execute(original)[0]["error"], "maintenance_indeterminate")

    def test_missing_decision_does_not_validate_a_modern_terminal(self):
        original = self.original(); result = self.execute(original)[0]
        self.assertEqual(result["effect_admission"], "admitted")
        directory = self.home / ".jash/agent_maintenance"
        decision = next(directory.glob("*.maintenance-decision.dsse")); raw = decision.read_bytes(); decision.unlink()
        self.assertEqual(self.execute(original)[0]["error"], "maintenance_indeterminate")
        self.assertFalse(self.request("store.resolve", target_id=original["id"])["ok"])
        decision.write_bytes(raw); decision.chmod(0o600)
        self.assertEqual(self.execute(original)[0], result)

    def test_gc_without_admission_keeps_unique_cas(self):
        self.request("store.inspect")
        orphan = fixture.Store.orphan(self)
        original = self.original("store.gc", older_than_seconds=0)
        self.kill_at(original, "JASH_TEST_MAINT_ADMISSION_GATE")
        self.assertTrue(orphan.exists())
        self.assertEqual(self.request("store.resolve", target_id=original["id"])["status"], "closed")
        self.assertTrue(orphan.exists()); self.assertFalse(self.execute(original)[0]["applied"])

    produce = fixture.Store.produce

    def test_release_without_admission_never_releases_root(self):
        self.produce()
        root = next(item for item in self.request("store.inspect")["items"] if item["kind"] == "job_archive")
        original = self.original("store.release", root_namespace=root["namespace"], root_name=root["name"], root_blake3=root["root_blake3"])
        self.kill_at(original, "JASH_TEST_MAINT_ADMISSION_GATE")
        self.assertTrue(self.request("store.resolve", target_id=original["id"])["ok"])
        after = next(item for item in self.request("store.inspect")["items"] if item["name"] == root["name"])
        self.assertEqual(after["retention"], "kept")
        self.assertFalse(list((self.home / ".jash/agent_maintenance").glob("*.release")))

    def test_closure_requires_exclusivity_and_preview_preserves_evidence(self):
        original = self.prepared(); directory = self.home / ".jash/agent_maintenance"
        before = {p.name: p.read_bytes() for p in directory.iterdir()}
        self.assertEqual(self.request("store.resolve", target_id=original["id"], dry_run=True)["status"], "closable")
        self.assertEqual(before, {p.name: p.read_bytes() for p in directory.iterdir()})
        peer = subprocess.Popen([str(fixture.AGENT)], cwd=self.work, env=self.env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            json.loads(peer.stdout.readline())
            self.assertEqual(self.request("store.resolve", target_id=original["id"])["error"], "store_busy")
            self.assertEqual(before, {p.name: p.read_bytes() for p in directory.iterdir()})
        finally:
            peer.stdin.close(); peer.stdin = None; peer.communicate(timeout=10)

    def test_partial_decision_never_allows_effect_or_closure(self):
        original = self.original()
        self.kill_at(original, "JASH_TEST_MAINT_ADMISSION_OPEN_GATE")
        self.assertFalse((self.home / ".jash/agent_store/quota").exists())
        decision = next((self.home / ".jash/agent_maintenance").glob("*.maintenance-decision.dsse"))
        self.assertEqual(decision.read_bytes(), b"")
        self.assertEqual(self.inspect(original["id"])["effect_admission"], "decision_invalid")
        self.assertFalse(self.request("store.resolve", target_id=original["id"])["ok"])
        self.assertEqual(decision.read_bytes(), b"")

    def test_closure_directory_sync_error_reports_visible_result(self):
        original = self.prepared()
        self.env.update(LD_PRELOAD=self.hook, JASH_TEST_MAINT_DIRSYNC_FAIL="1")
        failed = self.request("store.resolve", target_id=original["id"])
        self.env.pop("LD_PRELOAD"); self.env.pop("JASH_TEST_MAINT_DIRSYNC_FAIL")
        self.assertEqual(failed["error"], "closure_unconfirmed"); self.assertTrue(failed["published"])
        self.assertIsNotNone(failed["receipt_ref"])
        self.assertEqual(self.request("store.resolve", target_id=original["id"])["status"], "already_committed")
        self.assertEqual(self.execute(original)[0]["effect_admission"], "closed")

    def test_decision_verifier_binds_request_and_preparation(self):
        original = self.original(); result, wire, raw = self.execute(original)
        directory = self.home / ".jash/agent_maintenance"
        decision = next(directory.glob("*.maintenance-decision.dsse")); proof = decision.read_bytes()
        req = self.work / "request"; req.write_bytes(wire)
        command = [str(fixture.fixture.ROOT / "build/jef_verify"), "maintenance-decision", self.runtime_key, str(req), str(decision)]
        self.assertEqual(subprocess.run(command, capture_output=True, timeout=10).returncode, 0)
        for altered in (wire.replace(b"123456", b"123457"), wire.replace(b'{"id":', b'{"id":"duplicate","id":', 1)):
            req.write_bytes(altered)
            self.assertNotEqual(subprocess.run(command, capture_output=True, timeout=10).returncode, 0)
        req.write_bytes(wire)
        wrong = command.copy(); wrong[2] = "0" * 64
        self.assertEqual(subprocess.run(wrong, capture_output=True, timeout=10).returncode, 1)
        intent = next(directory.glob("*.maintenance-intent.dsse")); intent.write_bytes(b"{}")
        self.assertEqual(subprocess.run(command, capture_output=True, timeout=10).returncode, 1)


class AdmissionCore(unittest.TestCase):
    def test_legacy_compatibility_protocol_guards_and_closure(self):
        with tempfile.TemporaryDirectory(prefix="jash-admission-core-") as temp:
            binary = str(Path(temp) / "test")
            flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie"] if os.environ.get("SANITIZE") else []
            sources = ["tests/test_maintenance_admission_core.c", "c/core/agent_json.c", "c/core/json_writer.c", "c/core/recovery.c", "c/module/blake3.c",
                       "c/core/execution_ir.c", "c/core/agent_cgroup.c", "c/module/jash_crypto.c", "c/osif/posix_common.c", "libjef/jef_verify.c", "libjef/ed25519.c"]
            command = ["cc", "-std=gnu11", "-g", "-O1", "-ffunction-sections", "-fdata-sections", *flags]
            for include in ("c", "c/sys", "libjef"): command += ["-I", str(fixture.fixture.ROOT / include)]
            command += [str(fixture.fixture.ROOT / source) for source in sources] + ["-Wl,--gc-sections", "-o", binary]
            compiled = subprocess.run(command, capture_output=True, timeout=180)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            checked = subprocess.run([binary, temp], capture_output=True, timeout=10)
            self.assertEqual(checked.returncode, 0, checked.stderr)
            legacy = json.loads((Path(temp) / "legacy-inspection.json").read_bytes())
            self.assertEqual(legacy["effect_protocol"], "legacy"); self.assertEqual(legacy["state"], "unresolved")


class Inspection(unittest.TestCase):
    setUp = fixture.Store.setUp
    setUpClass = classmethod(fixture.Store.setUpClass.__func__)
    execute = Transactions.execute
    interrupted = Resolution.interrupted
    request = fixture.Store.request
    kill_at = Resolution.kill_at

    def inspect(self, target):
        return self.execute({"id": "inspect-target", "protocol_version": 2, "op": "store.inspect",
                             "target_id": target, "caps": ["store:read"]})[0]

    def test_pending_evidence_is_read_only_and_matches_resolution(self):
        original, _, candidate = self.interrupted()
        before = {p.name: (p.read_bytes(), p.stat().st_ino, p.stat().st_mtime_ns) for p in candidate.parent.iterdir()}
        view = self.inspect(original["id"])
        self.assertEqual(view.get("schema"), "jash.agent-maintenance-inspection.v1", view)
        self.assertEqual(view["state"], "recoverable"); self.assertTrue(view["resolution_eligible"])
        self.assertEqual(view["intent"]["receipt_status"], "verified")
        self.assertEqual(view["result"]["status"], "missing")
        self.assertFalse(view["maintenance_complete"]); self.assertTrue(view["scan_complete"])
        self.assertIn(candidate.name, [entry["ref"] for entry in view["files"]])
        self.assertEqual(before, {p.name: (p.read_bytes(), p.stat().st_ino, p.stat().st_mtime_ns) for p in candidate.parent.iterdir()})
        self.assertTrue(self.request("store.resolve", target_id=original["id"])["ok"])
        self.assertEqual(self.inspect(original["id"])["state"], "committed")

    def test_unknown_target_has_no_inferred_execution_state_or_journal_write(self):
        view = self.inspect("missing")
        self.assertTrue(view["ok"]); self.assertEqual(view["state"], "unresolved")
        self.assertEqual(view["reason"], "intent_missing"); self.assertFalse(view["resolution_eligible"])
        self.assertEqual(view["files"], []); self.assertEqual(view["intent"]["receipt_status"], "unknown")
        self.assertFalse((self.home / ".jash/agent_maintenance").exists())
        self.assertEqual(self.request("store.inspect")["schema"], "jash.agent-store.v1")

    def test_signed_intent_is_verifiable_without_terminal_and_does_not_clear_pending(self):
        original, wire, candidate = self.interrupted()
        candidate.unlink()
        for path in candidate.parent.glob("*.maintenance-terminal.dsse"): path.unlink()
        view = self.inspect(original["id"])
        self.assertEqual(view["intent"]["receipt_status"], "verified")
        self.assertEqual(view["operation"], "store.quota"); self.assertEqual(view["reason"], "terminal_unavailable")
        self.assertFalse(view["resolution_eligible"]); self.assertFalse(view["maintenance_complete"])
        req = self.work / "intent-request"; req.write_bytes(wire)
        checked = subprocess.run([str(fixture.fixture.ROOT / "build/jef_verify"), "maintenance-intent", self.runtime_key,
                                  str(req), str(candidate.parent / view["intent"]["receipt_ref"])], capture_output=True, timeout=10)
        self.assertEqual(checked.returncode, 0, checked.stderr)
        self.assertEqual(self.execute(original)[0]["error"], "maintenance_indeterminate")

    def test_killed_before_effect_reports_missing_signed_preparation(self):
        original = {"id": "before-inspect", "protocol_version": 2, "op": "store.quota", "max_bytes": 123456, "caps": ["store:manage"]}
        self.kill_at(original, "JASH_TEST_MAINT_INTENT_GATE")
        view = self.inspect(original["id"])
        self.assertEqual(view["intent"]["status"], "readable"); self.assertEqual(view["intent"]["receipt_status"], "missing")
        self.assertEqual(view["state"], "unresolved"); self.assertIsNone(view["operation"])
        self.assertFalse((self.home / ".jash/agent_store/quota").exists())
        self.assertEqual(self.request("store.resolve", target_id=original["id"])["error"], "terminal_unavailable")

    def test_unpublished_intent_staging_is_visible_without_reserving_id(self):
        original = {"id": "prepare-inspect", "protocol_version": 2, "op": "store.quota", "max_bytes": 123456, "caps": ["store:manage"]}
        self.env.update(LD_PRELOAD=self.hook, JASH_TEST_MAINT_PREPARE_FAIL="1")
        self.assertEqual(self.execute(original)[0]["error"], "maintenance_prepare_failed")
        self.env.pop("LD_PRELOAD"); self.env.pop("JASH_TEST_MAINT_PREPARE_FAIL")
        view = self.inspect(original["id"])
        self.assertEqual(view["intent"]["status"], "missing"); self.assertEqual(view["reason"], "intent_missing")
        self.assertEqual([entry["role"] for entry in view["files"]], ["intent_staging"])
        self.assertFalse((self.home / ".jash/agent_store/quota").exists())
        self.assertTrue(self.execute(original)[0]["ok"])

    def test_missing_intent_keeps_public_and_staging_evidence_visible(self):
        original, _, candidate = self.interrupted()
        self.assertTrue(self.request("store.resolve", target_id=original["id"])["ok"])
        next(candidate.parent.glob("*.intent")).unlink()
        view = self.inspect(original["id"])
        self.assertEqual(view["reason"], "intent_missing"); self.assertEqual(view["result"]["status"], "unverified")
        self.assertEqual({entry["role"] for entry in view["files"]}, {"result", "result_staging"})
        self.assertIsNone(view["terminal_receipt_ref"]); self.assertFalse(view["maintenance_complete"])

    def test_corrupt_public_result_identifies_blocking_file(self):
        original, _, candidate = self.interrupted()
        self.assertTrue(self.request("store.resolve", target_id=original["id"])["ok"])
        public = next(candidate.parent.glob("*.result")); public.write_bytes(b"corrupted")
        view = self.inspect(original["id"])
        self.assertEqual(view["reason"], "result_invalid"); self.assertEqual(view["blocked_ref"], public.name)
        self.assertEqual(view["result"]["status"], "invalid"); self.assertEqual(public.read_bytes(), b"corrupted")

    def test_candidate_failures_name_evidence_and_never_change_it(self):
        original, _, candidate = self.interrupted()
        raw = candidate.read_bytes()
        for fault in ("body", "permissions", "fifo", "symlink", "oversize"):
            with self.subTest(fault=fault):
                candidate.unlink(); candidate.write_bytes(raw); candidate.chmod(0o600)
                if fault == "body": candidate.write_bytes(raw.replace(b'"applied":true', b'"applied":false'))
                elif fault == "permissions": candidate.chmod(0o644)
                elif fault == "fifo": candidate.unlink(); os.mkfifo(candidate)
                elif fault == "symlink":
                    copy = self.work / "copy"; copy.write_bytes(raw); candidate.unlink(); candidate.symlink_to(copy)
                else:
                    with candidate.open("wb") as stream: stream.truncate(16 * 1024 * 1024 + 1)
                before = candidate.lstat()
                view = self.inspect(original["id"])
                self.assertEqual(view["blocked_ref"], candidate.name); self.assertFalse(view["resolution_eligible"])
                self.assertEqual(view["reason"], "candidate_limit" if fault == "oversize" else "candidate_invalid")
                entry = next(item for item in view["files"] if item["ref"] == candidate.name)
                self.assertEqual(entry["safety"], "unsafe" if fault in ("permissions", "fifo", "symlink") else "private_regular")
                self.assertEqual(candidate.lstat().st_ino, before.st_ino); self.assertFalse(list(candidate.parent.glob("*.result")))

    def test_missing_and_invalid_intent_receipts_are_distinguished(self):
        original, _, candidate = self.interrupted()
        receipt = next(candidate.parent.glob("*.maintenance-intent.dsse")); proof = receipt.read_bytes()
        receipt.unlink()
        self.assertEqual(self.inspect(original["id"])["intent"]["receipt_status"], "missing")
        receipt.write_bytes(b"{}"); receipt.chmod(0o600)
        self.assertEqual(self.inspect(original["id"])["intent"]["receipt_status"], "invalid")
        receipt.write_bytes(proof)
        self.assertEqual(self.inspect(original["id"])["intent"]["receipt_status"], "verified")

    def test_scan_limit_reports_incomplete_diagnostics_and_no_eligibility(self):
        original, _, candidate = self.interrupted()
        for i in range(4097): (candidate.parent / f".extra-{i}").touch(mode=0o600)
        view = self.inspect(original["id"])
        self.assertFalse(view["ok"]); self.assertEqual(view["error"], "scan_limit")
        self.assertEqual(view["entries_scanned"], 4096); self.assertFalse(view["scan_complete"])
        self.assertFalse(view["resolution_eligible"]); self.assertIsNone(view["blocked_ref"])
        self.assertTrue(candidate.exists()); self.assertFalse(list(candidate.parent.glob("*.result")))

    def test_read_only_inspection_with_peer_does_not_grant_resolution_authority(self):
        original, _, _ = self.interrupted()
        peer = subprocess.Popen([str(fixture.AGENT)], cwd=self.work, env=self.env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            json.loads(peer.stdout.readline())
            view = self.inspect(original["id"])
            self.assertTrue(view["resolution_eligible"]); self.assertEqual(view["eligibility_scope"], "evidence-only")
            self.assertEqual(self.request("store.resolve", target_id=original["id"])["error"], "store_busy")
            denied = self.execute({"id": "no-authority", "protocol_version": 2, "op": "store.resolve",
                                   "target_id": original["id"], "caps": ["store:read"]})[0]
            self.assertEqual(denied["error"], "capability denied")
        finally:
            peer.stdin.close(); peer.stdin = None; peer.communicate(timeout=10)

    def test_target_files_and_global_history_are_distinct(self):
        first = self.request("store.quota", max_bytes=200000)
        _, _, candidate = self.interrupted()
        view = self.inspect(first["id"])
        self.assertEqual(view["state"], "committed"); self.assertFalse(view["maintenance_complete"])
        self.assertTrue(view["resolution_eligible"]); self.assertEqual(view["result"]["status"], "verified")
        self.assertNotIn(candidate.name, [entry["ref"] for entry in view["files"]])

    def test_invalid_inspection_fields_fail_without_journal_changes(self):
        original, _, candidate = self.interrupted(identifier='"' * 47)
        request = {"id": "inspect", "protocol_version": 2, "op": "store.inspect", "target_id": original["id"], "caps": ["store:read"]}
        before = {p.name: p.read_bytes() for p in candidate.parent.iterdir()}
        for invalid in (dict(request, target_id=""), dict(request, target_id="x" * 48), dict(request, caps=[]), dict(request, dry_run=True)):
            self.assertIn("error", self.execute(invalid)[0])
        self.assertEqual(self.inspect(original["id"])["state"], "recoverable")
        self.assertEqual(before, {p.name: p.read_bytes() for p in candidate.parent.iterdir()})


class ResolutionCore(unittest.TestCase):
    setUp = fixture.Store.setUp
    setUpClass = classmethod(fixture.Store.setUpClass.__func__)
    execute = Transactions.execute
    interrupted = Resolution.interrupted

    def test_candidate_selection_and_publication_under_sanitizers(self):
        request, _, candidate = self.interrupted()
        with tempfile.TemporaryDirectory(prefix="jash-resolution-core-") as temp:
            binary = str(Path(temp) / "test")
            flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie"] if os.environ.get("SANITIZE") else []
            sources = ["tests/test_maintenance_resolution_core.c", "c/core/agent_json.c", "c/core/json_writer.c", "c/core/recovery.c", "c/module/blake3.c",
                       "c/module/jash_crypto.c", "c/osif/posix_common.c", "libjef/jef_verify.c", "libjef/ed25519.c"]
            command = ["cc", "-std=gnu11", "-g", "-O1", "-ffunction-sections", "-fdata-sections", *flags]
            for include in ("c", "c/sys", "libjef"): command += ["-I", str(fixture.fixture.ROOT / include)]
            command += [str(fixture.fixture.ROOT / source) for source in sources] + ["-Wl,--gc-sections", "-o", binary]
            compiled = subprocess.run(command, capture_output=True, timeout=180)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            checked = subprocess.run([binary, str(self.home), request["id"], self.runtime_key], capture_output=True, timeout=10)
            self.assertEqual(checked.returncode, 0, checked.stderr)
        self.assertEqual(self.execute(request)[2], candidate.read_bytes())


class VerifierCore(unittest.TestCase):
    setUp = fixture.Store.setUp
    execute = Transactions.execute

    def test_sanitized_binding_and_bounded_inputs(self):
        request = {"id": "native-proof", "protocol_version": 2, "op": "store.quota", "max_bytes": 100000, "caps": ["store:manage"]}
        result, wire, raw = self.execute(request)
        req = self.work / "request"; req.write_bytes(wire)
        res = self.work / "response"; res.write_bytes(raw)
        receipt = self.home / ".jash/agent_maintenance" / result["receipt_ref"]
        with tempfile.TemporaryDirectory(prefix="jash-maint-verifier-") as temporary:
            binary = str(Path(temporary) / "verify")
            flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie"] if os.environ.get("SANITIZE") else []
            sources = ["tests/test_maintenance_verifier.c", "libjef/jef_verify.c", "libjef/ed25519.c", "c/module/blake3.c", "c/module/jash_crypto.c"]
            command = ["cc", "-std=gnu11", "-g", "-O1", *flags, "-I", str(fixture.fixture.ROOT / "libjef"), "-I", str(fixture.fixture.ROOT / "c")]
            command += [str(fixture.fixture.ROOT / source) for source in sources] + ["-o", binary]
            compiled = subprocess.run(command, capture_output=True, timeout=180)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            arguments = [binary, str(receipt), str(res), str(req), self.runtime_key]
            self.assertEqual(subprocess.run(arguments, capture_output=True, timeout=10).returncode, 0)
            intent = next(receipt.parent.glob("*.maintenance-intent.dsse"))
            intent_args = [binary, str(intent), str(req), self.runtime_key]
            self.assertEqual(subprocess.run(intent_args, capture_output=True, timeout=10).returncode, 0)
            decision = next(receipt.parent.glob("*.maintenance-decision.dsse"))
            decision_args = [binary, "decision", str(decision), str(req), self.runtime_key]
            self.assertEqual(subprocess.run(decision_args, capture_output=True, timeout=10).returncode, 0)
            self.assertEqual(subprocess.run(decision_args[:-1] + ["0" * 64], capture_output=True, timeout=10).returncode, 1)
            self.assertEqual(subprocess.run(decision_args[:-1] + ["bad"], capture_output=True, timeout=10).returncode, 2)
            self.assertEqual(subprocess.run(intent_args[:-1] + ["0" * 64], capture_output=True, timeout=10).returncode, 1)
            self.assertEqual(subprocess.run(intent_args[:-1] + ["bad"], capture_output=True, timeout=10).returncode, 2)
            self.assertEqual(subprocess.run([binary, str(receipt), str(req), self.runtime_key], capture_output=True, timeout=10).returncode, 1)
            renamed = self.work / "wrong-name.dsse"; renamed.write_bytes(intent.read_bytes())
            self.assertEqual(subprocess.run([binary, str(renamed), str(req), self.runtime_key], capture_output=True, timeout=10).returncode, 1)
            for changed in (wire.replace(b"100000", b"200000"), wire.replace(b'{"id":', b'{"id":"duplicate","id":', 1), wire + b"{}"):
                req.write_bytes(changed)
                self.assertEqual(subprocess.run(intent_args, capture_output=True, timeout=10).returncode, 1)
                self.assertEqual(subprocess.run(decision_args, capture_output=True, timeout=10).returncode, 1)
            req.write_bytes(b" \t" + wire)
            self.assertEqual(subprocess.run(intent_args, capture_output=True, timeout=10).returncode, 0)
            with req.open("wb") as stream: stream.truncate(32769)
            self.assertEqual(subprocess.run(intent_args, capture_output=True, timeout=10).returncode, 2)
            self.assertEqual(subprocess.run(decision_args, capture_output=True, timeout=10).returncode, 2)
            req.unlink(); os.mkfifo(req)
            self.assertEqual(subprocess.run(intent_args, capture_output=True, timeout=10).returncode, 2)
            self.assertEqual(subprocess.run(decision_args, capture_output=True, timeout=10).returncode, 2)
            req.unlink(); req.write_bytes(wire)
            self.assertEqual(subprocess.run(arguments[:-1] + ["0" * 64], capture_output=True, timeout=10).returncode, 1)
            res.write_bytes(raw.replace(b'{"schema":', b'{"id":"duplicate","schema":', 1))
            self.assertEqual(subprocess.run(arguments, capture_output=True, timeout=10).returncode, 1)
            with res.open("wb") as stream: stream.truncate(16 * 1024 * 1024 + 1)
            self.assertEqual(subprocess.run(arguments, capture_output=True, timeout=10).returncode, 2)
            res.unlink(); os.mkfifo(res)
            self.assertEqual(subprocess.run(arguments, capture_output=True, timeout=10).returncode, 2)


if __name__ == "__main__": unittest.main()
