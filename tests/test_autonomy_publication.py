#!/usr/bin/env python3
"""Publication recovery against real receipt-backed candidates; no shared builds."""
import errno
import fcntl
import json
import multiprocessing
import os
from pathlib import Path
import shutil
import sys
import unittest
from unittest import mock
from argparse import Namespace

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_autonomy_workflow as fixtures

workflow = fixtures.workflow


class PublicationRecovery(unittest.TestCase):
    setUp = fixtures.WorkflowTests.setUp
    cli = fixtures.WorkflowTests.cli
    run_workflow = fixtures.WorkflowTests.run_workflow
    verify = fixtures.WorkflowTests.verify

    def prepare(self):
        self.run_workflow()
        self.destination = self.root / "published"
        self.args = Namespace(repo=str(self.repo), destination=str(self.destination),
                              bundle=str(self.bundle), pubkey=self.key)
        self.result, self.artifact = workflow.verify_bundle(
            self.bundle, self.key, fixtures.VERIFIER)

    def recover(self, rc=0):
        return self.verify("--repo", self.repo, "--destination", self.destination,
                           command="recover", rc=rc)

    def transaction(self):
        return self.destination.parent / (
            ".autonomy-publication-" + workflow.sha(os.fsencode(self.destination.name)))

    def crash(self, phase):
        def child():
            sync = workflow.publication_sync
            chmod = workflow.os.fchmod

            def stop(fd, label):
                sync(fd, label)
                if label == phase:
                    os._exit(86)

            def stop_mode(fd, mode):
                if phase == "commit-mode" and mode == 0o555:
                    os._exit(86)
                chmod(fd, mode)

            with mock.patch.object(workflow, "publication_sync", side_effect=stop), \
                 mock.patch.object(workflow.os, "fchmod", side_effect=stop_mode):
                try:
                    workflow.publish(self.args, self.result, self.artifact)
                except BaseException:
                    os._exit(87)
            os._exit(88)

        process = multiprocessing.get_context("fork").Process(target=child)
        process.start()
        process.join(15)
        if process.is_alive():
            process.kill()
            process.join(5)
            self.fail("publication process did not reach injected crash")
        self.assertEqual(process.exitcode, 86, phase)

    def test_process_death_before_preparation_never_publishes(self):
        self.prepare()
        for index, phase in enumerate(("claim-parent", "write-candidate",
                                       "write-manifest.json", "prepare-payload")):
            with self.subTest(phase=phase):
                self.destination = self.root / ("early-" + str(index))
                self.args.destination = str(self.destination)
                self.crash(phase)
                self.assertFalse(self.destination.exists())
                transaction = self.transaction()
                before = {p.name: p.read_bytes() for p in transaction.iterdir() if p.is_file()}
                result = self.recover(rc=1)
                self.assertEqual(result["publication"]["state"], "incomplete")
                self.assertFalse(self.destination.exists())
                self.assertEqual(before, {p.name: p.read_bytes() for p in transaction.iterdir()
                                          if p.is_file()})

    def test_process_death_at_prepared_and_commit_boundaries_recovers(self):
        self.prepare()
        for index, phase in enumerate(("write-prepared.json", "prepare-transaction",
                                       "prepare-parent", "retry-prepare-parent",
                                       "commit-mode", "commit-payload", "commit-parent", "commit-transaction")):
            with self.subTest(phase=phase):
                self.destination = self.root / ("boundary-" + str(index))
                self.args.destination = str(self.destination)
                self.crash(phase)
                prepared = json.loads((self.transaction() / "prepared.json").read_bytes())
                result = self.recover()
                self.assertEqual(result["publication"]["state"], "committed")
                self.assertEqual([self.destination.stat().st_dev, self.destination.stat().st_ino],
                                 prepared["payload"])
                self.assertEqual((self.destination / "candidate").read_bytes(), self.artifact)
                self.assertEqual(self.destination.stat().st_mode & 0o777, 0o555)
                self.assertEqual(self.recover()["publication"]["state"], "committed")
        self.assertEqual((self.repo / "target").read_bytes(), b"baseline\n")

    def test_prepared_recovery_refuses_stale_source_then_can_resume(self):
        self.prepare()
        self.crash("prepare-parent")
        (self.repo / "target").write_bytes(b"changed\n")
        result = self.recover(rc=1)
        self.assertEqual(result["publication"]["state"], "prepared")
        self.assertFalse(self.destination.exists())
        self.assertTrue((self.transaction() / "payload/candidate").exists())
        (self.repo / "target").write_bytes(b"baseline\n")
        self.recover()

    def test_owned_rename_reconciles_even_when_source_is_gone(self):
        self.prepare()
        self.crash("commit-payload")
        inode = self.destination.stat().st_ino
        shutil.rmtree(self.repo)
        result = self.recover()
        self.assertEqual(result["publication"]["state"], "committed")
        self.assertFalse(result["publication"]["baseline_current"])
        self.assertEqual(self.destination.stat().st_ino, inode)

    def test_recovery_io_failure_remains_indeterminate_and_retryable(self):
        self.prepare()
        self.crash("commit-payload")
        sync = workflow.publication_sync

        def fail(fd, label):
            if label == "read-candidate":
                raise OSError(errno.EIO, "recovery read fsync failed")
            sync(fd, label)

        with mock.patch.object(workflow, "publication_sync", side_effect=fail):
            with self.assertRaises(workflow.PublicationError) as raised:
                workflow.publish(self.args, self.result, self.artifact, recover=True)
        self.assertEqual(raised.exception.publication["state"], "indeterminate")
        self.assertEqual((self.destination / "candidate").read_bytes(), self.artifact)
        self.recover()

    def test_corrupt_or_foreign_intent_preserves_error_context_and_evidence(self):
        self.prepare()
        self.crash("prepare-parent")
        path = self.transaction() / "prepared.json"
        original = path.read_bytes()
        record = json.loads(original)
        for corrupt in (b"{", b"[" * 2000 + b"]" * 2000,
                        workflow.wire({**record, "evidence_sha256": "0" * 64}),
                        workflow.wire({**record, "pubkey": "0" * 64}),
                        workflow.wire({**record, "destination": str(self.root / "foreign")})):
            with self.subTest(corrupt=corrupt[:20]):
                path.write_bytes(corrupt)
                result = self.recover(rc=1)
                self.assertTrue(result["verified"])
                self.assertEqual(result["publication"]["transaction"], str(self.transaction()))
                self.assertFalse(self.destination.exists())
                self.assertEqual(path.read_bytes(), corrupt)
        path.write_bytes(original)
        self.recover()

    def test_existing_destination_and_pending_payload_are_preserved(self):
        self.prepare()
        self.crash("prepare-parent")
        self.destination.mkdir()
        (self.destination / "sentinel").write_bytes(b"foreign")
        result = self.recover(rc=1)
        self.assertEqual(result["publication"]["state"], "conflict")
        self.assertEqual((self.destination / "sentinel").read_bytes(), b"foreign")
        self.assertEqual((self.transaction() / "payload/candidate").read_bytes(), self.artifact)

    def test_missing_both_locations_never_reconstructs_output(self):
        self.prepare()
        self.crash("prepare-parent")
        (self.transaction() / "payload").rename(self.root / "saved-payload")
        self.recover(rc=1)
        self.assertFalse(self.destination.exists())
        self.assertFalse((self.transaction() / "payload").exists())

    def test_transaction_lock_is_stable_and_nonblocking(self):
        self.prepare()
        self.crash("prepare-parent")
        path = self.transaction() / "lock"
        inode = path.stat().st_ino
        with path.open("r+b") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            self.recover(rc=1)
            self.assertFalse(self.destination.exists())
        self.recover()
        self.assertEqual(path.stat().st_ino, inode)

    def test_foreign_key_and_modified_bundle_cannot_trigger_recovery(self):
        self.prepare()
        self.crash("prepare-parent")
        expected = self.key
        self.key = "0" * 64
        result = self.recover(rc=1)
        self.assertFalse(result["verified"])
        self.assertFalse(self.destination.exists())
        self.key = expected
        artifact = self.bundle / "c0/artifact"
        original = artifact.read_bytes()
        artifact.write_bytes(b"unverified")
        self.assertFalse(self.recover(rc=1)["verified"])
        self.assertFalse(self.destination.exists())
        artifact.write_bytes(original)
        self.recover()

    def test_symlinked_transaction_or_destination_is_not_followed(self):
        self.prepare()
        self.crash("prepare-parent")
        original = self.root / "saved-transaction"
        self.transaction().rename(original)
        self.transaction().symlink_to(original, target_is_directory=True)
        self.recover(rc=1)
        self.assertFalse(self.destination.exists())
        self.transaction().unlink()
        original.rename(self.transaction())
        self.destination.symlink_to(self.repo, target_is_directory=True)
        self.assertEqual(self.recover(rc=1)["publication"]["state"], "conflict")
        self.assertTrue(self.destination.is_symlink())

    def test_group_writable_publication_parent_is_rejected(self):
        self.prepare()
        self.root.chmod(0o770)
        try:
            with self.assertRaises(workflow.PublicationError):
                workflow.publish(self.args, self.result, self.artifact)
            self.assertFalse(self.destination.exists())
            self.assertFalse(self.transaction().exists())
        finally:
            self.root.chmod(0o700)

    def test_concurrent_publishers_and_recovery_never_replace_winner(self):
        self.prepare()

        def run_pair(recover):
            context = multiprocessing.get_context("fork")
            children, readers = [], []
            for _ in range(2):
                receive, send = context.Pipe(duplex=False)

                def child(channel=send):
                    try:
                        result = workflow.publish(self.args, self.result, self.artifact, recover=recover)
                        channel.send(result["publication"]["state"])
                    except workflow.PublicationError as exc:
                        channel.send(exc.publication["state"])
                    finally:
                        channel.close()

                process = context.Process(target=child)
                process.start()
                send.close()
                children.append(process)
                readers.append(receive)
            try:
                for process in children:
                    process.join(15)
                    self.assertFalse(process.is_alive(), "publisher did not finish")
                    self.assertEqual(process.exitcode, 0)
                self.assertTrue(all(reader.poll(2) for reader in readers))
                return [reader.recv() for reader in readers]
            finally:
                for process in children:
                    if process.is_alive():
                        process.kill()
                        process.join(5)
                for reader in readers:
                    reader.close()

        states = run_pair(False)
        self.assertEqual(states.count("committed"), 1)
        self.assertEqual(states.count("conflict"), 1)
        inode = self.destination.stat().st_ino
        states = run_pair(True)
        self.assertIn("committed", states)
        self.assertEqual(self.destination.stat().st_ino, inode)
        self.assertEqual((self.destination / "candidate").read_bytes(), self.artifact)

    def test_parent_substitution_cannot_redirect_publication(self):
        self.prepare()
        parent = self.root / "destination-parent"
        parent.mkdir(mode=0o700)
        self.destination = parent / "published"
        self.args.destination = str(self.destination)
        displaced = self.root / "displaced-parent"
        sync = workflow.publication_sync

        def replace(fd, label):
            sync(fd, label)
            if label == "prepare-parent":
                parent.rename(displaced)
                parent.mkdir(mode=0o700)
                (parent / "sentinel").write_bytes(b"foreign parent")

        with mock.patch.object(workflow, "publication_sync", side_effect=replace):
            with self.assertRaises(workflow.PublicationError) as raised:
                workflow.publish(self.args, self.result, self.artifact)
        self.assertEqual(raised.exception.publication["state"], "conflict")
        self.assertFalse(self.destination.exists())
        self.assertFalse((displaced / "published").exists())
        self.assertEqual((parent / "sentinel").read_bytes(), b"foreign parent")
        self.assertTrue(list(displaced.glob(".autonomy-publication-*/payload/candidate")))

    def test_recover_after_rename_and_parent_fsync_failure(self):
        self.prepare()
        real_sync = os.fsync

        def interrupted(fd):
            if self.destination.exists():
                raise OSError(errno.EIO, "injected failure after rename")
            return real_sync(fd)

        with mock.patch.object(workflow.os, "fsync", side_effect=interrupted):
            with self.assertRaises(OSError):
                workflow.publish(self.args, self.result, self.artifact)
        self.assertEqual((self.destination / "candidate").read_bytes(), self.artifact)
        inode = self.destination.stat().st_ino
        result = self.recover()
        self.assertEqual(result["publication"]["state"], "committed")
        self.assertEqual(self.destination.stat().st_ino, inode)
        self.assertEqual((self.repo / "target").read_bytes(), b"baseline\n")

    def test_recovery_is_idempotent_and_does_not_replace_output(self):
        self.prepare()
        workflow.publish(self.args, self.result, self.artifact)
        inode = self.destination.stat().st_ino
        for _ in range(2):
            result = self.recover()
            self.assertEqual(result["publication"]["state"], "committed")
            self.assertEqual(self.destination.stat().st_ino, inode)

    def test_identical_foreign_directory_is_not_adopted(self):
        self.prepare()
        workflow.publish(self.args, self.result, self.artifact)
        original = self.root / "original-output"
        self.destination.rename(original)
        shutil.copytree(original, self.destination)
        replacement = self.destination.stat().st_ino
        result = self.recover(rc=1)
        self.assertEqual(result["publication"]["state"], "conflict")
        self.assertEqual(self.destination.stat().st_ino, replacement)
        self.assertEqual((original / "candidate").read_bytes(), self.artifact)


if __name__ == "__main__":
    unittest.main(verbosity=2)
