#!/usr/bin/env python3
"""Real binaries, no rebuild: python3 -B tests/test_autonomy_workflow.py."""
import json
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools import jash_autonomy as workflow

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "tools/jash_autonomy.py"
BUILD = Path(os.environ.get("JASH_BUILD_DIR", ROOT / "build"))
AGENT = os.environ.get("JASH_AGENTD_BIN", str(BUILD / "jash-agentd"))
VERIFIER = os.environ.get("JASH_VERIFIER_BIN", str(BUILD / "jef_verify"))


class WorkflowTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="autonomy-test-")
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.repo = self.root / "repo"
        self.repo.mkdir()
        subprocess.run(["git", "init", "-q", str(self.repo)], check=True)
        (self.repo / "target").write_bytes(b"baseline\n")
        (self.repo / "other").write_bytes(b"untouched\n")
        subprocess.run(["git", "-C", str(self.repo), "add", "target", "other"], check=True)
        (self.repo / "secret-untracked").write_text("not an input")
        self.source_files = {p.relative_to(self.repo).as_posix(): (p.read_bytes(), p.stat().st_mode)
                             for p in self.repo.rglob("*") if p.is_file()}
        self.bundle = self.root / "bundle"
        self.home = self.root / "runtime"

    def cli(self, *args, rc=0):
        result = subprocess.run([sys.executable, "-B", str(SCRIPT), *map(str, args)],
                                capture_output=True, text=True, timeout=45)
        self.assertEqual(result.returncode, rc, result.stdout + result.stderr)
        return json.loads(result.stdout)

    def run_workflow(self, generators=None, check=None, timeout=2000):
        generators = generators or [["/bin/sh", "-c", "printf 'candidate\\n' > target"]]
        check = check or ["/bin/sh", "-c", "test ! -e .git && test ! -e secret-untracked && test -s target"]
        args = ["run", "--repo", self.repo, "--target", "target", "--bundle", self.bundle,
                "--runtime-home", self.home, "--agentd", AGENT, "--verifier", VERIFIER,
                "--timeout-ms", str(timeout), "--check", json.dumps(check)]
        for generator in generators:
            args += ["--generate", json.dumps(generator)]
        result = self.cli(*args)
        self.key = result["local_pubkey"]
        self.assertEqual((self.repo / "target").read_bytes(), b"baseline\n")
        self.assertEqual((self.repo / "other").read_bytes(), b"untouched\n")
        self.assertEqual({p.relative_to(self.repo).as_posix(): (p.read_bytes(), p.stat().st_mode)
                          for p in self.repo.rglob("*") if p.is_file()}, self.source_files)
        return result

    def verify(self, *extra, rc=0, command="verify"):
        return self.cli(command, "--bundle", self.bundle, "--pubkey", self.key,
                        "--verifier", VERIFIER, *extra, rc=rc)

    def test_happy_portable_and_publish(self):
        result = self.run_workflow()
        self.assertEqual(result["winner"], "c0")
        moved = self.root / "moved"
        self.bundle.rename(moved)
        self.bundle = moved
        shutil.rmtree(self.home)
        self.assertEqual(self.verify()["winner"], "c0")
        destination = self.root / "published"
        self.verify("--repo", self.repo, "--destination", destination, command="publish")
        self.assertEqual((destination / "candidate").read_bytes(), b"candidate\n")
        self.assertEqual((destination / "candidate").stat().st_mode & 0o222, 0)
        manifest = json.loads((destination / "manifest.json").read_bytes())
        self.assertEqual(manifest["status"], "done")
        self.assertEqual(manifest["pubkey"], self.key)
        self.assertEqual((self.repo / "target").read_bytes(), b"baseline\n")

    def test_binary_artifact_and_streams_are_exact(self):
        result = self.run_workflow(generators=[["/usr/bin/python3", "-c",
            "import pathlib; pathlib.Path('target').write_bytes(b'\\x00\\xff\\n')"]],
            check=["/usr/bin/python3", "-c",
                   "import pathlib,os; assert pathlib.Path('target').read_bytes()==b'\\x00\\xff\\n'; "
                   "os.write(1,b'OUT\\x00\\xff'); os.write(2,b'ERR\\x00')"])
        self.assertEqual(result["winner"], "c0")
        self.assertEqual((self.bundle / "c0/artifact").read_bytes(), b"\x00\xff\n")
        self.assertEqual({p.read_bytes() for p in (self.bundle / "c0/check/overflow").iterdir()},
                         {b"OUT\x00\xff", b"ERR\x00"})
        self.verify()

    def test_tampered_evidence_index_and_foreign_key(self):
        self.run_workflow()
        phase = self.bundle / "c0/check"
        files = [phase / "spec.json", self.bundle / "c0/artifact",
                 phase / "start-request.json", phase / "terminal-response.json",
                 next((phase / "agent_receipts").iterdir())]
        for path in files:
            with self.subTest(path=path.name):
                original = path.read_bytes()
                path.write_bytes(original + b" ")
                self.verify(rc=1)
                path.write_bytes(original)
        index = self.bundle / "index.json"
        original = index.read_bytes()
        for changed in ({"schema": workflow.SCHEMA, "candidates": ["../c0"]},
                        {"schema": workflow.SCHEMA, "candidates": ["c0"], "winner": "c0"},
                        {"schema": workflow.SCHEMA, "candidates": ["c0"], "check": ["true"]}):
            index.write_text(json.dumps(changed))
            self.verify(rc=1)
        index.write_bytes(b'{"schema":"' + workflow.SCHEMA.encode() +
                          b'","candidates":["c0"],"candidates":["c0"]}')
        self.verify(rc=1)
        index.write_bytes(original)
        self.key = "0" * 64
        self.verify(rc=1)
        self.assertEqual(self.verify(command="handoff", rc=1)["status"], "invalidated")

    def test_unknown_phase_metadata_rejected(self):
        self.run_workflow()
        (self.bundle / "c0/check/eligibility.json").write_text('{"eligible":true}')
        self.verify(rc=1)

    def test_missing_stream_and_symlinks_rejected(self):
        self.run_workflow(check=["/bin/sh", "-c", "printf checked"])
        stream = next((self.bundle / "c0/check/overflow").iterdir())
        data = stream.read_bytes()
        stream.unlink()
        self.verify(rc=1)
        stream.write_bytes(data)
        artifact = self.bundle / "c0/artifact"
        data = artifact.read_bytes()
        artifact.unlink()
        outside = self.root / "outside"
        outside.write_bytes(data)
        artifact.symlink_to(outside)
        self.verify(rc=1)
        artifact.unlink()
        artifact.write_bytes(data)
        phase = self.bundle / "c0/check"
        moved = self.root / "phase"
        phase.rename(moved)
        phase.symlink_to(moved, target_is_directory=True)
        self.verify(rc=1)

    def test_failed_generator_and_checker_have_evidence_but_no_publication(self):
        result = self.run_workflow(generators=[["/bin/sh", "-c", "printf failed > target; exit 7"]],
                                   check=["/bin/sh", "-c", "exit 8"])
        self.assertIsNone(result["winner"])
        row = self.verify()["evidence"][0]
        self.assertEqual((row["generation_rc"], row["check_rc"], row["eligible"]), (7, 8, False))
        destination = self.root / "never"
        self.verify("--repo", self.repo, "--destination", destination, command="publish", rc=1)
        self.assertFalse(destination.exists())

    def test_failed_generator_alone_cannot_pass(self):
        result = self.run_workflow(generators=[["/bin/false"]])
        self.assertIsNone(result["winner"])
        self.assertEqual(result["evidence"][0]["check_rc"], 0)

    def test_failed_checker_alone_cannot_pass(self):
        result = self.run_workflow(check=["/bin/false"])
        self.assertIsNone(result["winner"])
        self.assertEqual(result["evidence"][0]["generation_rc"], 0)

    def test_two_candidates_policy_and_omission(self):
        result = self.run_workflow(generators=[
            ["/bin/sh", "-c", "printf longer > target"],
            ["/bin/sh", "-c", "sleep 0.1; printf x > target"]])
        self.assertEqual(result["winner"], "c1")
        index = self.bundle / "index.json"
        original = index.read_bytes()
        shutil.rmtree(self.bundle / "c1")
        index.write_text(json.dumps({"schema": workflow.SCHEMA, "candidates": ["c0"]}))
        self.assertIn("omission", self.verify(rc=1)["error"])
        index.write_bytes(original)
        self.verify(rc=1)

    def test_equal_length_uses_sha256_not_completion_order(self):
        result = self.run_workflow(generators=[
            ["/bin/sh", "-c", "printf b > target"],
            ["/bin/sh", "-c", "printf a > target"]])
        expected = min([(hashlib.sha256(b"b").hexdigest(), "c0"),
                        (hashlib.sha256(b"a").hexdigest(), "c1")])[1]
        self.assertEqual(result["winner"], expected)

    def test_all_candidates_fail(self):
        result = self.run_workflow(generators=[["/bin/false"], ["/bin/false"]])
        self.assertIsNone(result["winner"])
        self.assertEqual(len(self.verify()["evidence"]), 2)

    def test_generation_cannot_change_other_files_or_add_files(self):
        result = self.run_workflow(generators=[
            ["/bin/sh", "-c", "printf candidate > target; printf changed > other"],
            ["/bin/sh", "-c", "printf candidate > target; touch extra"]])
        self.assertIsNone(result["winner"])
        self.assertTrue(all(not e["generation_valid"] for e in result["evidence"]))
        self.assertTrue(all(e["artifact_source"] == "baseline-fallback-ineligible"
                            for e in result["evidence"]))

    def test_target_type_changes_are_not_candidates(self):
        result = self.run_workflow(generators=[
            ["/bin/sh", "-c", "rm target; ln -s other target"],
            ["/bin/sh", "-c", "rm target; mkdir target"]])
        self.assertIsNone(result["winner"])
        self.assertTrue(all(not e["generation_valid"] for e in result["evidence"]))

    def test_nontarget_metadata_and_workspace_mode_changes_rejected(self):
        result = self.run_workflow(generators=[
            ["/bin/sh", "-c", "touch other"],
            ["/bin/sh", "-c", "chmod 777 /work"]])
        self.assertIsNone(result["winner"])
        self.assertTrue(all(not e["generation_valid"] for e in result["evidence"]))

    def test_check_is_clean_readonly_and_environment_is_sanitized(self):
        payload = ("import os,pathlib; p=pathlib.Path; "
                   "assert 'AUTONOMY_SECRET' not in os.environ; "
                   "assert os.environ['HOME']=='/tmp'; "
                   "assert not p('.git').exists(); assert not p('secret-untracked').exists(); "
                   "assert not p('/tmp/generator-marker').exists(); "
                   "p('/tmp/check-marker').write_text('private')\n"
                   "try: p('target').write_text('bad')\n"
                   "except OSError: pass\nelse: raise AssertionError('writable check')")
        with mock.patch.dict(os.environ, {"AUTONOMY_SECRET": "must-not-inherit"}):
            result = self.run_workflow(generators=[
                ["/bin/sh", "-c", "touch /tmp/generator-marker; printf candidate > target"]],
                check=["/usr/bin/python3", "-c", payload])
        self.assertEqual(result["winner"], "c0")

    def test_handoff_and_stale_source_blocks_publish(self):
        self.run_workflow()
        self.assertEqual(self.verify(command="handoff")["status"], "unknown")
        result = self.verify("--repo", self.repo, command="handoff")
        self.assertEqual(result["status"], "current")
        self.assertTrue(result["not_verified"])
        (self.repo / "other").write_bytes(b"stale\n")
        self.assertEqual(self.verify("--repo", self.repo, command="handoff")["status"], "invalidated")
        self.verify("--repo", self.repo, "--destination", self.root / "never", command="publish", rc=1)
        self.assertEqual((self.repo / "other").read_bytes(), b"stale\n")

    def test_preexisting_destination_untouched_and_source_destination_rejected(self):
        self.run_workflow()
        dest = self.root / "existing"
        dest.mkdir()
        (dest / "candidate").write_bytes(b"existing")
        self.verify("--repo", self.repo, "--destination", dest, command="promote", rc=1)
        self.assertEqual((dest / "candidate").read_bytes(), b"existing")
        self.verify("--repo", self.repo, "--destination", self.repo / "publication",
                    command="publish", rc=1)
        self.assertFalse((self.repo / "publication").exists())

    def test_timeouts_are_not_passes(self):
        result = self.run_workflow(generators=[["/bin/sleep", "5"]],
                                   check=["/bin/sleep", "5"], timeout=40)
        self.assertIsNone(result["winner"])
        self.assertFalse(self.verify()["evidence"][0]["eligible"])
        self.assertTrue((self.bundle / "c0/check/cancel-request.json").exists())

    def test_missing_sandbox_refuses_without_fallback(self):
        from argparse import Namespace
        args = Namespace(repo=str(self.repo), target="target", bundle=str(self.bundle),
                         runtime_home=str(self.home), agentd=AGENT, verifier=VERIFIER,
                         generate=['["/bin/true"]'], check='["/bin/true"]', timeout_ms=1000)
        with mock.patch.object(workflow, "build_argv", side_effect=FileNotFoundError("no bwrap")):
            with self.assertRaisesRegex(FileNotFoundError, "no bwrap"):
                workflow.run(args)
        self.assertFalse((self.bundle / "index.json").exists())
        self.assertEqual((self.repo / "target").read_bytes(), b"baseline\n")

    def test_snapshot_rejects_symlink_absent_unreadable_and_bounds(self):
        target = self.repo / "target"
        for kind in ("symlink", "absent", "unreadable", "oversize"):
            with self.subTest(kind=kind):
                if target.exists() or target.is_symlink():
                    target.unlink()
                if kind == "symlink":
                    target.symlink_to("other")
                elif kind == "unreadable":
                    target.write_bytes(b"secret")
                    target.chmod(0)
                elif kind == "oversize":
                    target.write_bytes(b"x" * (workflow.FILE_MAX + 1))
                with self.assertRaises((OSError, ValueError)):
                    workflow.snapshot(self.repo)

    def test_strict_json_and_paths(self):
        for raw in (b'{"a":1,"a":2}', b'{"a":NaN}', b'{"a":1}\x00'):
            with self.assertRaises(ValueError):
                workflow.decode(raw)
        for path in ("../target", "/target", "a//b", "a/./b", ".git/config", "a/../b", "a\x00"):
            with self.assertRaises(ValueError):
                workflow.relative(path)

    def test_snapshot_detects_same_bytes_rewritten_during_capture(self):
        original_read = workflow.read_file
        rewritten = False

        def read(root, name, *args):
            nonlocal rewritten
            result = original_read(root, name, *args)
            if name == "target" and not rewritten:
                rewritten = True
                (self.repo / "other").write_bytes(b"untouched\n")
            return result

        with mock.patch.object(workflow, "read_file", side_effect=read):
            with self.assertRaisesRegex(ValueError, "metadata changed"):
                workflow.snapshot(self.repo)

    def test_tracked_gitlink_and_symlink_entries_rejected(self):
        (self.repo / "link").symlink_to("target")
        subprocess.run(["git", "-C", str(self.repo), "add", "link"], check=True)
        with self.assertRaisesRegex(ValueError, "tracked symlink, gitlink"):
            workflow.snapshot(self.repo)
        subprocess.run(["git", "-C", str(self.repo), "update-index", "--force-remove", "link"],
                       check=True)
        subprocess.run(["git", "-C", str(self.repo), "update-index", "--add", "--cacheinfo",
                        "160000," + "1" * 40 + ",submodule"], check=True)
        with self.assertRaisesRegex(ValueError, "tracked symlink, gitlink"):
            workflow.snapshot(self.repo)

    def test_output_limit_and_indeterminate_are_not_passes(self):
        good = dict(state="done", rc=0, signal=0, terminal=True, quiescent=True,
                    cancel_requested=False, output_limit=False, quiescence_forced=False, kill_sent=False)
        request = {"id": "c0-check-wait"}
        self.assertTrue(workflow.job_passed(good, request, "c0-check"))
        for key, value in (("state", "indeterminate"), ("rc", False), ("terminal", False),
                           ("quiescent", False), ("cancel_requested", True), ("output_limit", True),
                           ("quiescence_forced", True), ("kill_sent", True), ("signal", 9)):
            with self.subTest(key=key):
                self.assertFalse(workflow.job_passed({**good, key: value}, request, "c0-check"))

    def test_publication_racing_destination_is_not_replaced(self):
        from argparse import Namespace
        result = self.run_workflow()
        artifact = (self.bundle / "c0/artifact").read_bytes()
        destination = self.root / "race"
        args = Namespace(repo=self.repo, destination=destination, bundle=self.bundle, pubkey=self.key)
        calls = 0

        def unchanged(*_):
            nonlocal calls
            calls += 1
            if calls == 2:
                destination.mkdir()
                (destination / "sentinel").write_bytes(b"do not replace")
            return True

        with mock.patch.object(workflow, "current", side_effect=unchanged):
            with self.assertRaises(workflow.PublicationError) as raised:
                workflow.publish(args, result, artifact)
            self.assertEqual(raised.exception.publication["state"], "conflict")
        self.assertEqual((destination / "sentinel").read_bytes(), b"do not replace")
        self.assertFalse((destination / "candidate").exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
