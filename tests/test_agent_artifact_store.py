#!/usr/bin/env python3
"""Artifact maintenance preserves durable references and refuses active peers."""
import json
import os
from pathlib import Path
import subprocess
import unittest
import shutil
import tempfile
import shlex
import test_agent_local_cancel as local
import test_agent_mutations_json as fixture

AGENT = Path(os.environ.get("JASH_AGENTD_BIN", fixture.ROOT / "build/jash-agentd")).resolve()


class Store(unittest.TestCase):
    setUp = fixture.Mutations.setUp
    setUpClass = classmethod(local.LocalCancel.setUpClass.__func__)

    def produce(self, text="retained", home=None, good=True):
        request = {"id": "producer-" + text, "protocol_version": 2, "op": "exec", "argv": ["/usr/bin/printf", text],
                   "caps": ["effect:exec", "job:create"]}
        p = subprocess.run([str(AGENT)], cwd=self.work, env=dict(self.env, HOME=str(home or self.home)),
                           input=(json.dumps(request) + "\n").encode(), capture_output=True, timeout=20)
        if good: self.assertEqual(p.returncode, 0, p.stderr)
        else: self.assertEqual(p.returncode, 125, p.stderr)
        return json.loads(p.stdout.splitlines()[-1])

    def orphan(self):
        with tempfile.TemporaryDirectory(prefix="jash-orphan-source-") as temp:
            produced = self.produce("orphan", home=Path(temp))
            source = Path(temp) / ".jash/overflow" / produced["stdout_ref"]
            directory = self.home / ".jash/overflow"; directory.mkdir(mode=0o700, parents=True, exist_ok=True)
            target = directory / source.name; shutil.copyfile(source, target); target.chmod(0o600)
            return target

    def request(self, op, **fields):
        self.request_count = getattr(self, "request_count", 0) + 1
        request = {"id": f"maintenance-{self.request_count}", "protocol_version": 2, "op": op,
                   "caps": ["store:read", "store:manage"], **fields}
        p = subprocess.run([str(AGENT)], cwd=self.work, env=self.env,
                           input=(json.dumps(request) + "\n").encode(), capture_output=True, timeout=20)
        self.assertEqual(p.returncode, 0, p.stderr)
        return json.loads(p.stdout.splitlines()[-1])

    def test_empty_store_inspection_and_explicit_quota(self):
        inspected = self.request("store.inspect")
        self.assertEqual(inspected.get("schema"), "jash.agent-store.v1", inspected)
        self.assertTrue(inspected["ok"])
        self.assertEqual(inspected["quota_bytes"], 0)
        self.assertEqual(inspected["used_bytes"], 0)
        configured = self.request("store.quota", max_bytes=4096)
        self.assertTrue(configured["ok"], configured)
        self.assertEqual(self.request("store.inspect")["quota_bytes"], 4096)

    def test_gc_preserves_roots_and_reclaims_only_orphan(self):
        produced = self.produce(); orphan = self.orphan()
        retained = self.home / ".jash/overflow" / produced["stdout_ref"]
        before = {p: p.read_bytes() for name in ("agent_requests", "agent_receipts", "agent_job_results")
                  for p in (self.home / ".jash" / name).iterdir()}
        dry = self.request("store.gc", older_than_seconds=0, dry_run=True)
        self.assertTrue(dry["ok"], (dry, [p.read_bytes()[:256] for p in (self.home / ".jash/agent_requests").iterdir()])); self.assertEqual(dry["planned_files"], 1)
        self.assertTrue(orphan.exists())
        done = self.request("store.gc", older_than_seconds=0)
        self.assertTrue(done["ok"], done); self.assertEqual(done["reclaimed_files"], 1)
        self.assertTrue(done["applied"])
        self.assertFalse(orphan.exists()); self.assertEqual(retained.read_bytes(), b"retained")
        self.assertEqual(before, {p: p.read_bytes() for p in before})

    def test_active_peer_blocks_gc(self):
        self.request("store.inspect"); orphan = self.orphan()
        peer = subprocess.Popen([str(AGENT)], cwd=self.work, env=self.env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            json.loads(peer.stdout.readline())
            result = self.request("store.gc", older_than_seconds=0)
            self.assertFalse(result["ok"]); self.assertEqual(result["error"], "store_busy")
            self.assertTrue(orphan.exists())
        finally:
            peer.stdin.close(); peer.stdin = None; peer.communicate(timeout=10)
        result = self.request("store.gc", older_than_seconds=0)
        self.assertTrue(result["ok"], result)
        self.assertFalse(orphan.exists())

    def test_corrupt_receipt_and_pending_request_block_deletion(self):
        self.produce(); orphan = self.orphan()
        receipt = next((self.home / ".jash/agent_receipts").iterdir()); original = receipt.read_bytes()
        receipt.write_bytes(b"{}")
        result = self.request("store.gc", older_than_seconds=0)
        self.assertFalse(result["scan_complete"]); self.assertTrue(orphan.exists())
        receipt.write_bytes(original)
        pending = self.home / ".jash/agent_requests" / ("a" * 64 + ".req")
        pending.write_text("b" * 64 + "\nrunning\n"); pending.chmod(0o600)
        self.assertFalse(self.request("store.gc", older_than_seconds=0)["ok"])
        self.assertTrue(orphan.exists())

    def test_quota_refuses_admission_without_deleting_recovery(self):
        self.produce(); before = self.request("store.inspect")
        self.assertTrue(self.request("store.quota", max_bytes=before["used_bytes"])["ok"])
        failed = self.produce("additional-output", good=False)
        self.assertEqual(failed["outcome"], "indeterminate")
        self.assertTrue(list((self.home / ".jash/agent_jobs").glob("*.stdout")))
        self.assertTrue(self.request("store.quota", max_bytes=0)["ok"])

    def test_quota_preview_and_below_usage_are_non_destructive(self):
        self.produce()
        rejected = self.request("store.quota", max_bytes=1)
        self.assertFalse(rejected["ok"]); self.assertEqual(rejected["error"], "quota_below_usage")
        self.assertEqual(self.request("store.inspect")["quota_bytes"], 0)
        planned = self.request("store.quota", max_bytes=100000, dry_run=True)
        self.assertTrue(planned["ok"]); self.assertFalse(planned["applied"])
        self.assertEqual(self.request("store.inspect")["quota_bytes"], 0)

    def test_gc_restores_referenced_capture_and_removes_duplicate_staging(self):
        produced = self.produce()
        target = self.home / ".jash/overflow" / produced["stdout_ref"]
        held = target.with_name(".gc-" + target.name); target.rename(held)
        repaired = self.request("store.gc", older_than_seconds=0)
        self.assertTrue(repaired["ok"], repaired); self.assertEqual(repaired["reconciled_files"], 1)
        self.assertEqual(target.read_bytes(), b"retained")
        digest = target.name.split("-")[1]
        staging = target.with_name(f".cas-{digest}-99999999-1.tmp")
        shutil.copyfile(target, staging); staging.chmod(0o600)
        result = self.request("store.gc", older_than_seconds=0)
        self.assertTrue(result["ok"], result); self.assertFalse(staging.exists())
        self.assertTrue(target.exists())

    def test_failed_gc_unlink_is_reconciled_on_next_attempt(self):
        self.request("store.inspect"); orphan = self.orphan()
        self.env.update(LD_PRELOAD=self.hook, JASH_TEST_STORE_UNLINK_FAIL="1")
        result = self.request("store.gc", older_than_seconds=0)
        self.assertFalse(result["ok"], result)
        held = orphan.with_name(".gc-" + orphan.name); self.assertTrue(held.exists())
        self.env.pop("LD_PRELOAD")
        retried = self.request("store.gc", older_than_seconds=0)
        self.assertTrue(retried["ok"], retried); self.assertFalse(held.exists())

    def test_redundant_spool_is_reclaimed_but_unresolved_spool_is_preserved(self):
        produced = self.produce()
        source = self.home / ".jash/overflow" / produced["stdout_ref"]
        jobs = self.home / ".jash/agent_jobs"; jobs.mkdir(mode=0o700, exist_ok=True)
        spool = jobs / (produced["job_id"] + ".stdout")
        shutil.copyfile(source, spool); spool.chmod(0o600)
        result = self.request("store.gc", older_than_seconds=0)
        self.assertTrue(result["ok"], result); self.assertFalse(spool.exists())
        unknown = jobs / ("j-" + "0" * 32 + ".stdout"); unknown.write_bytes(b"only recovery copy"); unknown.chmod(0o600)
        result = self.request("store.gc", older_than_seconds=0)
        self.assertFalse(result["ok"]); self.assertTrue(unknown.exists())

    def test_concurrent_admission_stays_within_limit(self):
        self.assertTrue(self.request("store.quota", max_bytes=5000)["ok"])
        processes = []
        for name in ("a", "b"):
            request = {"id": "concurrent-" + name, "protocol_version": 2, "op": "exec",
                       "argv": ["/usr/bin/python3", "-c", f"print('{name}'*1000,end='')"], "caps": ["effect:exec", "job:create"]}
            p = subprocess.Popen([str(AGENT)], cwd=self.work, env=self.env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            p.stdin.write((json.dumps(request) + "\n").encode()); p.stdin.close(); p.stdin = None; processes.append(p)
        results = [p.communicate(timeout=20) for p in processes]
        self.assertEqual(sorted(p.returncode for p in processes), [0, 125], results)
        state = self.request("store.inspect")
        self.assertLessEqual(state["used_bytes"], 5000)

    def test_undo_reference_protects_an_otherwise_orphaned_cas(self):
        self.request("store.inspect"); orphan = self.orphan()
        source = self.work / "source"; source.write_bytes(orphan.read_bytes())
        p = subprocess.run([str(fixture.JASH), "-c", shlex.join(["cp", "--json", str(source), str(orphan)])],
                           cwd=self.work, env=dict(self.env, JASH_TIMELINE="1"), capture_output=True, timeout=20)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIsNotNone(json.loads(p.stdout)["items"][0]["undo"])
        result = self.request("store.gc", older_than_seconds=0)
        self.assertTrue(result["ok"], result); self.assertTrue(orphan.exists())
        other = self.work / "other-home"; other.mkdir()
        requests = [{"id": "home", "argv": ["export", "HOME=" + str(other)], "caps": ["effect:mutate"]},
                    {"id": "gc", "protocol_version": 2, "op": "store.gc", "older_than_seconds": 0, "caps": ["store:manage"]}]
        result = subprocess.run([str(AGENT)], cwd=self.work, env=self.env,
            input=("\n".join(json.dumps(r) for r in requests) + "\n").encode(), capture_output=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(json.loads(result.stdout.splitlines()[-1])["ok"])
        self.assertTrue(orphan.exists())

    def test_failed_header_does_not_apply_gc_or_quota(self):
        self.request("store.inspect"); orphan = self.orphan()
        for request in ({"op": "store.gc", "older_than_seconds": 0}, {"op": "store.quota", "max_bytes": 100000}):
            request.update(id="full", protocol_version=2, caps=["store:manage"])
            with open("/dev/full", "wb", buffering=0) as sink:
                subprocess.run([str(AGENT)], cwd=self.work, env=self.env, input=(json.dumps(request) + "\n").encode(),
                               stdout=sink, stderr=subprocess.PIPE, timeout=20)
        self.assertTrue(orphan.exists())
        self.assertEqual(self.request("store.inspect")["quota_bytes"], 0)

    def test_storage_root_is_pinned_across_shell_home_changes(self):
        other = self.work / "other"; other.mkdir()
        requests = [{"id": "home", "argv": ["export", "HOME=" + str(other)], "caps": ["effect:mutate"]},
                    {"id": "inspect", "protocol_version": 2, "op": "store.inspect", "caps": ["store:read"]}]
        p = subprocess.run([str(AGENT)], cwd=self.work, env=self.env, input=("\n".join(json.dumps(r) for r in requests) + "\n").encode(),
                           capture_output=True, timeout=20)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertTrue(json.loads(p.stdout.splitlines()[-1])["ok"])
        self.assertFalse((other / ".jash").exists())

    def test_capture_identity_change_preserves_both_versions(self):
        self.request("store.inspect"); orphan = self.orphan()
        self.env.update(LD_PRELOAD=self.hook, JASH_TEST_STORE_CAPTURE_SWAP="1")
        result = self.request("store.gc", older_than_seconds=0)
        self.assertFalse(result["ok"])
        self.assertEqual(orphan.read_bytes(), b"raced")
        self.assertEqual((orphan.parent / "saved-original").read_bytes(), b"orphan")
        self.assertIn("identity_changed", [item["action"] for item in result["items"]])

    def test_store_permissions_validation_age_and_corrupt_quota(self):
        self.request("store.inspect"); orphan = self.orphan()
        self.assertTrue(self.request("store.gc")["ok"]); self.assertTrue(orphan.exists())
        for fields in ({"caps": ["job:read"]}, {"caps": ["store:manage"], "older_than_seconds": -1},
                       {"caps": ["store:manage"], "argv": ["rm", str(orphan)]}):
            request = {"id": "bad", "protocol_version": 2, "op": "store.gc", **fields}
            p = subprocess.run([str(AGENT)], cwd=self.work, env=self.env, input=(json.dumps(request) + "\n").encode(), capture_output=True, timeout=10)
            self.assertIn("error", json.loads(p.stdout.splitlines()[-1]))
            self.assertTrue(orphan.exists())
        self.assertTrue(self.request("store.quota", max_bytes=100000)["ok"])
        (self.home / ".jash/agent_store/quota").write_bytes(b"0\0ignored\n")
        bad = self.request("store.inspect")
        self.assertFalse(bad["ok"]); self.assertIsNone(bad["quota_bytes"])

    def test_session_with_jobs_cannot_upgrade_to_maintenance(self):
        proc = subprocess.Popen([str(AGENT)], cwd=self.work, env=self.env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            json.loads(proc.stdout.readline())
            requests = [
                {"id": "job", "protocol_version": 2, "op": "job.start", "argv": ["sleep", "60"], "background": True, "caps": ["effect:exec", "job:create"]},
                {"id": "gc", "protocol_version": 2, "op": "store.gc", "older_than_seconds": 0, "caps": ["store:manage"]}]
            results = []
            for request in requests:
                proc.stdin.write((json.dumps(request) + "\n").encode()); proc.stdin.flush(); results.append(json.loads(proc.stdout.readline()))
            self.assertIn("job_id", results[0])
            self.assertEqual(results[1]["error"], "session_has_jobs")
        finally:
            proc.stdin.close(); proc.stdin = None; proc.communicate(timeout=10)

    def test_scan_limit_is_explicit_and_prevents_prefix_only_gc(self):
        self.request("store.inspect"); orphan = self.orphan()
        roots = self.home / ".jash/agent_requests"; roots.mkdir(mode=0o700)
        for index in range(4097):
            path = roots / f"unqualified-{index}.req"; path.write_bytes(b"pending"); path.chmod(0o600)
        result = self.request("store.gc", older_than_seconds=0)
        self.assertFalse(result["scan_complete"])
        self.assertEqual(result["entries_scanned"], 4096)
        self.assertEqual(result["reclaimed_files"], 0)
        self.assertTrue(orphan.exists())


class StoreCore(unittest.TestCase):
    def test_scan_bounds_preview_and_reference_fail_closed(self):
        with tempfile.TemporaryDirectory(prefix="jash-store-core-") as temp:
            binary = str(Path(temp) / "test")
            flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie"] if os.environ.get("SANITIZE") else []
            sources = ["tests/test_artifact_store_core.c", "c/core/json_writer.c", "c/core/recovery.c", "c/module/blake3.c",
                       "c/module/jash_crypto.c", "c/osif/posix_common.c", "libjef/jef_verify.c", "libjef/ed25519.c"]
            command = ["cc", "-std=gnu11", "-g", "-O1", "-ffunction-sections", "-fdata-sections", *flags]
            for include in ("c", "c/sys", "libjef"): command += ["-I", str(fixture.ROOT / include)]
            command += [str(fixture.ROOT / source) for source in sources] + ["-Wl,--gc-sections", "-o", binary]
            compiled = subprocess.run(command, capture_output=True, timeout=180)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            result = subprocess.run([binary, temp], capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__": unittest.main()
