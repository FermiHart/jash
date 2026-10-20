#!/usr/bin/env python3
"""ADR-0056 W03: compare native manifests with actual filesystem effects."""
import base64
import json
import os
from pathlib import Path
import shlex
import signal
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
JASH = Path(os.environ.get("JASH", ROOT / "build/jash")).resolve()


def path_bytes(value):
    return value.encode() if isinstance(value, str) else base64.b64decode(value["data"], validate=True)


class Mutations(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory(prefix="jash-mutations-")
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        self.home = self.root / "home"
        self.home.mkdir()
        self.work = self.root / "work"
        self.work.mkdir()
        self.env = {k: v for k, v in os.environ.items() if not k.startswith("JASH_")}
        self.env.update(HOME=str(self.home), JASH_SIGIL="0", JASH_TIMELINE="0",
                        JASH_MEMO="0", JASH_NUDGE="0", JASH_ADOPT="0", JASH_BRIEF="0",
                        JASH_LOOPGUARD="off", JASH_WORLD="0", JASH_PREFETCH="0")

    def runj(self, command, timeout=15, **kwargs):
        return subprocess.run([str(JASH), "-c", command], cwd=self.work, env=self.env,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout, **kwargs)

    def manifest(self, *args, rc=0, input=None):
        result = self.runj(shlex.join(args), input=input)
        self.assertEqual(result.returncode, rc, result.stderr)
        data = json.loads(result.stdout)
        self.check_report(data, rc)
        return data

    def check_report(self, data, rc):
        self.assertEqual(data["schema"], "jash.mutation.v1")
        self.assertEqual(data["exit_code"], rc)
        self.assertEqual(data["undo_refs"], "validated_local")
        counts = {k: 0 for k in ("completed", "skipped", "failed", "planned", "partial")}
        for item in data["items"]:
            counts[item["status"]] += 1
            self.assertIsNone(item["undo"])
            if item["status"] in ("skipped", "planned"):
                self.assertEqual(item["effect"], "none")
        self.assertEqual(data["counts"], counts)

    def test_copy_bytes_verify_and_paths(self):
        names = [b'quote"back\\line\n\t', "á∞😀".encode(), b"raw-\xff-\x80"]
        (self.work / "src").write_bytes(b"payload\x00bytes")
        for name in names:
            data = self.manifest("cp", "--json", "--verify", "src", os.fsdecode(name))
            item, = data["items"]
            self.assertEqual(path_bytes(item["destination"]), name)
            self.assertEqual(item["status"], "completed")
            self.assertEqual(item["bytes_written"], 13)
            self.assertTrue(item["verified"])
            self.assertEqual((self.work / os.fsdecode(name)).read_bytes(), b"payload\x00bytes")

    def test_copy_update_uses_nanoseconds(self):
        src, dst = self.work / "src", self.work / "dst"
        src.write_text("new")
        dst.write_text("old")
        ns = 1_700_000_000_000_000_000
        os.utime(src, ns=(ns, ns + 1))
        os.utime(dst, ns=(ns, ns + 2))
        data = self.manifest("cp", "--json", "-u", "src", "dst")
        self.assertEqual(data["items"][0]["status"], "skipped")
        self.assertEqual(dst.read_text(), "old")
        os.utime(src, ns=(ns, ns + 3))
        self.manifest("cp", "--json", "-u", "src", "dst")
        self.assertEqual(dst.read_text(), "new")

    def test_copy_no_clobber_and_same_file(self):
        (self.work / "src").write_text("source")
        (self.work / "dst").write_text("keep")
        data = self.manifest("cp", "--json", "-n", "src", "dst")
        self.assertEqual(data["counts"]["skipped"], 1)
        self.assertEqual((self.work / "dst").read_text(), "keep")
        data = self.manifest("cp", "--json", "src", "src", rc=1)
        self.assertEqual(data["items"][0]["effect"], "none")
        self.assertEqual((self.work / "src").read_text(), "source")

    def test_copy_mixed_results(self):
        (self.work / "src").write_text("source")
        (self.work / "dst").mkdir()
        data = self.manifest("cp", "--json", "missing", "src", "dst", rc=1)
        self.assertEqual(data["status"], "partial")
        self.assertEqual([i["status"] for i in data["items"]], ["failed", "completed"])
        self.assertEqual((self.work / "dst/src").read_text(), "source")

    def test_copy_tree_preview_and_apply(self):
        (self.work / "src").mkdir()
        (self.work / "src/file").write_text("data")
        (self.work / "src/link").symlink_to("missing")
        data = self.manifest("cp", "--json", "-a", "--dry-run", "src", "dst")
        self.assertEqual(data["status"], "planned")
        self.assertFalse((self.work / "dst").exists())
        data = self.manifest("cp", "--json", "-a", "src", "dst")
        self.assertEqual(data["counts"]["failed"], 0)
        self.assertEqual((self.work / "dst/file").read_text(), "data")
        self.assertEqual(os.readlink(self.work / "dst/link"), "missing")

    def test_json_flag_boundary(self):
        (self.work / "--json").write_text("literal operand")
        result = self.runj("cp -- --json dst")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, b"")
        self.assertEqual((self.work / "dst").read_text(), "literal operand")
        self.manifest("cp", "--json", "--", "--json", "second")
        self.assertEqual((self.work / "second").read_text(), "literal operand")

    def test_copy_sink_error_precedes_effect(self):
        (self.work / "src").write_text("source")
        result = self.runj("cp --json src dst > /dev/full")
        self.assertEqual(result.returncode, 125, result.stderr)
        self.assertFalse((self.work / "dst").exists())

    def test_copy_output_routes_and_small_budget(self):
        (self.work / "src").write_text("source")
        self.env.update(JASH_LLM_BUDGET="64", JASH_MEMO="1")
        for command in ("cp --json src dst", "true; cp --json src dst",
                        "cp --json src dst | /bin/cat"):
            with self.subTest(command=command):
                result = self.runj(command)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.check_report(json.loads(result.stdout), 0)
        result = self.runj("cp --json src dst > report")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.check_report(json.loads((self.work / "report").read_bytes()), 0)
        # The outer printf is ordinary text and may be capped; inspect the
        # captured value through a file destination instead of that text cap.
        result = self.runj('r=$(cp --json src dst); printf "%s\\n" "$r" > captured')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.check_report(json.loads((self.work / "captured").read_bytes()), 0)

    def test_move_directory_preview_is_noop(self):
        (self.work / "src").mkdir()
        (self.work / "src/file").write_text("keep")
        result = self.runj("mv --dry-run src dst")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue((self.work / "src/file").exists())
        self.assertFalse((self.work / "dst").exists())

    def test_move_dangling_symlink(self):
        (self.work / "src").symlink_to("missing")
        result = self.runj("mv src dst")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(os.readlink(self.work / "dst"), "missing")
        self.assertFalse(os.path.lexists(self.work / "src"))

    def test_remove_preview_does_not_expire_trash(self):
        trash = self.home / ".jash/trash"
        trash.mkdir(parents=True)
        old = trash / "123_old"
        old.write_text("recoverable")
        os.utime(old, (1, 1))
        (self.work / "src").write_text("keep")
        result = self.runj("rm --undo --dry-run src")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.work / "src").read_text(), "keep")
        self.assertTrue(old.exists())

    def test_move_results_and_conflict(self):
        (self.work / "src").write_text("keep")
        (self.work / "dst").mkdir()
        (self.work / "dst/src").write_text("existing")
        data = self.manifest("mv", "--json", "-n", "missing", "src", "dst", rc=1)
        self.assertEqual([i["status"] for i in data["items"]], ["failed", "skipped"])
        self.assertEqual((self.work / "src").read_text(), "keep")
        self.assertEqual((self.work / "dst/src").read_text(), "existing")
        data = self.manifest("mv", "--json", "-f", "src", "dst")
        self.assertEqual(data["items"][0]["effect"], "changed")
        self.assertEqual(data["items"][0]["bytes_written"], 0)
        self.assertFalse((self.work / "src").exists())
        self.assertEqual((self.work / "dst/src").read_text(), "keep")

    def test_move_preview_json_and_dangling_link(self):
        (self.work / "src").symlink_to("missing")
        data = self.manifest("mv", "--json", "--dry-run", "src", "dst")
        self.assertEqual(data["items"][0]["status"], "planned")
        self.assertFalse(os.path.lexists(self.work / "dst"))
        data = self.manifest("mv", "--json", "src", "dst")
        self.assertEqual(data["items"][0]["status"], "completed")
        self.assertEqual(os.readlink(self.work / "dst"), "missing")

    def test_move_target_directory_forms(self):
        for flags in (("-t", "dst"), ("-tdst",), ("-vt", "dst"),
                      ("--target-directory=dst",), ("--target-directory", "dst")):
            with self.subTest(flags=flags):
                (self.work / "src").write_text("data")
                (self.work / "dst").mkdir(exist_ok=True)
                data = self.manifest("mv", "--json", *flags, "src")
                self.assertEqual(data["items"][0]["destination"], "dst/src")
                self.assertEqual((self.work / "dst/src").read_text(), "data")
                (self.work / "dst/src").unlink()

    def test_move_option_value_is_not_json_flag(self):
        (self.work / "--json").mkdir()
        for flags in (("-t", "--json"), ("-vt", "--json"), ("-t--json",)):
            (self.work / "src").write_text("data")
            result = self.runj(shlex.join(["mv", *flags, "src"]))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertNotIn(b'"schema":', result.stdout)
            self.assertEqual((self.work / "--json/src").read_text(), "data")
            (self.work / "--json/src").unlink()
        (self.work / "src").write_text("data")
        self.manifest("mv", "--json", "-vt", "--json", "src")

    def test_move_no_target_directory_and_invalid_combinations(self):
        (self.work / "src").write_text("keep")
        (self.work / "dst").mkdir()
        for flags in (("-T",), ("--no-target-directory",), ("-T", "-t", "dst")):
            data = self.manifest("mv", "--json", *flags, "src", "dst", rc=1)
            self.assertEqual((self.work / "src").read_text(), "keep")
            self.assertFalse((self.work / "dst/src").exists())
        (self.work / "src").unlink()
        (self.work / "src").mkdir()
        (self.work / "src/file").write_text("data")
        self.manifest("mv", "--json", "-T", "src", "dst")
        self.assertEqual((self.work / "dst/file").read_text(), "data")

    def test_move_trailing_slash_directory_operand(self):
        (self.work / "src").mkdir()
        (self.work / "src/file").write_text("data")
        (self.work / "dst").mkdir()
        self.manifest("mv", "--json", "src/", "dst")
        self.assertEqual((self.work / "dst/src/file").read_text(), "data")
        self.assertFalse((self.work / "src").exists())

    def test_move_directory_declined_prompt(self):
        (self.work / "src").mkdir()
        (self.work / "src/file").write_text("keep")
        (self.work / "dst").mkdir()
        data = self.manifest("mv", "--json", "-Ti", "src", "dst", input=b"no\n")
        self.assertEqual(data["items"][0]["status"], "skipped")
        self.assertEqual((self.work / "src/file").read_text(), "keep")
        self.assertEqual(list((self.work / "dst").iterdir()), [])

    def test_copy_fifo_preview_never_waits_for_a_writer(self):
        os.mkfifo(self.work / "src")
        result = self.runj("cp --json --dry-run src dst", timeout=1)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)["status"], "planned")
        self.assertFalse((self.work / "dst").exists())

    def test_remove_results_and_force_absent(self):
        (self.work / "src").write_text("data")
        data = self.manifest("rm", "--json", "missing", "src", rc=1)
        self.assertEqual(data["status"], "partial")
        self.assertEqual([i["status"] for i in data["items"]], ["failed", "completed"])
        self.assertFalse((self.work / "src").exists())
        data = self.manifest("rm", "--json", "-f", "missing")
        self.assertEqual(data["items"][0]["status"], "skipped")
        self.manifest("rm", "--json", "-f")

    def test_remove_tree_and_symlink_do_not_follow_target(self):
        (self.work / "outside").mkdir()
        (self.work / "outside/file").write_text("keep")
        (self.work / "tree").mkdir()
        (self.work / "tree/link").symlink_to("../outside")
        data = self.manifest("rm", "--json", "-r", "--dry-run", "tree")
        self.assertEqual(data["status"], "planned")
        self.assertTrue((self.work / "tree/link").is_symlink())
        data = self.manifest("rm", "--json", "-r", "tree")
        self.assertEqual(data["items"][0]["effect"], "changed")
        self.assertFalse((self.work / "tree").exists())
        self.assertEqual((self.work / "outside/file").read_text(), "keep")

    def test_remove_trash_and_preview_have_no_unrelated_expiration(self):
        trash = self.home / ".jash/trash"
        trash.mkdir(parents=True)
        old = trash / "123_old"
        old.write_text("old")
        os.utime(old, (1, 1))
        (self.work / "src").write_text("recover")
        data = self.manifest("rm", "--json", "--undo", "src")
        self.assertEqual(data["items"][0]["action"], "trash")
        self.assertFalse((self.work / "src").exists())
        self.assertTrue(any(p.read_bytes() == b"recover" for p in trash.iterdir()))
        self.assertTrue(old.exists())

    def test_mutators_declined_prompts(self):
        for command in ("mv", "rm"):
            (self.work / "src").write_text("keep")
            (self.work / "dst").write_text("existing")
            args = ("src", "dst") if command == "mv" else ("src",)
            data = self.manifest(command, "--json", "-i", *args, input=b"no\n")
            self.assertEqual(data["items"][0]["status"], "skipped")
            self.assertEqual((self.work / "src").read_text(), "keep")
            self.assertEqual((self.work / "dst").read_text(), "existing")

    def test_mutators_sink_error_precedes_effect(self):
        (self.work / "src").write_text("keep")
        for command in ("mv --json src dst", "rm --json --undo src"):
            result = self.runj(command + " > /dev/full")
            self.assertEqual(result.returncode, 125, result.stderr)
            self.assertEqual((self.work / "src").read_text(), "keep")
            self.assertFalse((self.work / "dst").exists())
            self.assertFalse((self.home / ".jash/trash").exists())

    def test_copy_failure_after_partial_output_stops_next_operand(self):
        fifo = self.work / "fifo"
        os.mkfifo(fifo)
        (self.work / "second").write_text("second")
        (self.work / "dst").mkdir()
        p = subprocess.Popen([str(JASH), "-c", "cp --json fifo second dst"],
                             cwd=self.work, env=self.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.addCleanup(lambda: p.poll() is None and p.kill())
        header = b""
        while not header.endswith(b'"items":['):
            byte = p.stdout.read(1)
            self.assertTrue(byte, header)
            header += byte
        p.stdout.close()
        with open(fifo, "wb", buffering=0) as out:
            out.write(b"first")
        self.assertEqual(p.wait(timeout=10), 125, p.stderr.read())
        p.stderr.close()
        self.assertEqual((self.work / "dst/fifo").read_text(), "first")
        self.assertFalse((self.work / "dst/second").exists())

    def test_copy_cancellation_in_multi_source_operation(self):
        fifo = self.work / "fifo"
        os.mkfifo(fifo)
        (self.work / "second").write_text("second")
        (self.work / "dst").mkdir()
        p = subprocess.Popen([str(JASH), "-c", "cp --json fifo second dst"],
                             cwd=self.work, env=self.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.addCleanup(lambda: p.poll() is None and p.kill())
        with open(fifo, "wb", buffering=0):
            deadline = time.monotonic() + 5
            while not (self.work / "dst/fifo").exists() and time.monotonic() < deadline:
                time.sleep(.005)
            self.assertTrue((self.work / "dst/fifo").exists())
            p.send_signal(signal.SIGINT)
            stdout, stderr = p.communicate(timeout=10)
        self.assertEqual(p.returncode, 130, stderr)
        data = json.loads(stdout)
        self.check_report(data, 130)
        self.assertEqual(data["status"], "cancelled")
        self.assertEqual(data["items"][0]["status"], "partial")
        self.assertFalse((self.work / "dst/second").exists())

    def test_copy_sigterm_closes_a_partial_report(self):
        fifo = self.work / "fifo"; os.mkfifo(fifo)
        (self.work / "second").write_text("keep")
        (self.work / "dst").mkdir()
        p = subprocess.Popen([str(JASH), "-c", "cp --json fifo second dst"], cwd=self.work, env=self.env,
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.addCleanup(lambda: p.poll() is None and p.kill())
        with open(fifo, "wb", buffering=0):
            deadline = time.monotonic() + 5
            while not (self.work / "dst/fifo").exists() and time.monotonic() < deadline: time.sleep(.005)
            self.assertTrue((self.work / "dst/fifo").exists())
            p.send_signal(signal.SIGTERM)
            stdout, stderr = p.communicate(timeout=10)
        self.assertEqual(p.returncode, 143, stderr)
        data = json.loads(stdout)
        self.check_report(data, 143)
        self.assertEqual(data["status"], "cancelled")
        self.assertFalse((self.work / "dst/second").exists())

    def test_large_copy_manifest_is_not_truncated(self):
        names = [f"source-{i:02}-" + "x" * 140 for i in range(60)]
        for name in names: (self.work / name).write_text(name)
        (self.work / "dst").mkdir()
        self.env["JASH_LLM_BUDGET"] = "64"
        data = self.manifest("cp", "--json", *names, "dst")
        self.assertEqual(len(data["items"]), len(names))
        self.assertGreater(len(json.dumps(data)), 16384)
        for name in names: self.assertEqual((self.work / "dst" / name).read_text(), name)

    def test_move_cross_device_preserves_nanoseconds(self):
        if not os.path.isdir("/dev/shm") or os.stat("/dev/shm").st_dev == self.work.stat().st_dev:
            self.skipTest("requires a writable distinct /dev/shm filesystem")
        with tempfile.TemporaryDirectory(prefix="jash-mut-exdev-", dir="/dev/shm") as tmp:
            src = self.work / "src"
            src.write_text("data")
            ns = 1_700_000_000_123_456_789
            os.utime(src, ns=(ns, ns))
            data = self.manifest("mv", "--json", "src", tmp + "/dst")
            self.assertEqual(data["items"][0]["bytes_written"], 4)
            self.assertEqual(Path(tmp + "/dst").stat().st_mtime_ns, ns)
            self.assertEqual(Path(tmp + "/dst").read_text(), "data")
            self.assertFalse(src.exists())

    def test_remove_partial_tree_is_not_reported_as_success(self):
        if os.geteuid() == 0: self.skipTest("permission fixture requires unprivileged user")
        tree = self.work / "tree"
        tree.mkdir()
        (tree / "removed").write_text("data")
        locked = tree / "locked"
        locked.mkdir()
        (locked / "kept").write_text("keep")
        locked.chmod(0)
        self.addCleanup(locked.chmod, 0o700)
        data = self.manifest("rm", "--json", "-r", "tree", rc=1)
        self.assertEqual(data["status"], "partial")
        self.assertEqual(data["items"][0]["effect"], "changed")
        self.assertFalse((tree / "removed").exists())
        locked.chmod(0o700)
        self.assertEqual((locked / "kept").read_text(), "keep")

    def test_exec_json_and_serve_keep_manifest_as_command_output(self):
        (self.work / "src").write_text("data")
        for command in ("cp", "mv", "rm"):
            argv = [command, "--json", "src", "dst"] if command != "rm" else [command, "--json", "dst"]
            req = {"id": command, "argv": argv}
            if command == "cp":
                p = subprocess.run([str(JASH), "--exec-json", json.dumps({"argv": argv})], cwd=self.work,
                                   env=self.env, capture_output=True, timeout=15)
                self.assertEqual(p.returncode, 0, p.stderr)
                self.check_report(json.loads(p.stdout), 0)
            else:
                p = subprocess.run([str(JASH), "--serve"], cwd=self.work, env=self.env,
                                   input=(json.dumps(req) + "\n").encode(), capture_output=True, timeout=15)
                self.assertEqual(p.returncode, 0, p.stderr)
                rows = [json.loads(row) for row in p.stdout.splitlines()]
                self.assertIn("rc", rows[-1], rows)
                self.assertEqual(rows[-1]["rc"], 0, rows)
                self.check_report(json.loads(rows[-1]["stdout"]), 0)
        self.assertFalse((self.work / "dst").exists())


class ReportCore(unittest.TestCase):
    def test_native_metadata_stops_after_failed_item_delivery(self):
        with tempfile.TemporaryDirectory(prefix="jash-meta-unit-build-") as tmp:
            binary = str(Path(tmp) / "metadata")
            files = [ROOT / "tests/test_metadata_report.c", ROOT / "c/builtins/builtin_metaops.c",
                     ROOT / "c/core/mutation_report.c", ROOT / "c/core/json_writer.c"]
            files += [ROOT / path for path in ["c/core/data_value.c", "c/core/data_stream.c",
                      "c/core/data_wire.c", "c/core/data_wire_decode.c", "c/module/blake3.c"]]
            command = [os.environ.get("CC", "cc"), "-std=gnu11", "-D_GNU_SOURCE", "-O1", "-g",
                       "-I" + str(ROOT / "c"), *map(str, files), "-o", binary]
            if os.environ.get("SANITIZE") == "1":
                command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
            built = subprocess.run(command, capture_output=True, timeout=30)
            self.assertEqual(built.returncode, 0, built.stderr)
            result = subprocess.run([binary], capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_short_writes_failures_enums_and_signal_restoration(self):
        with tempfile.TemporaryDirectory(prefix="jash-report-unit-") as tmp:
            binary = str(Path(tmp) / "report")
            files = [ROOT / "tests/test_mutation_report.c", ROOT / "c/core/mutation_report.c",
                     ROOT / "c/core/json_writer.c"]
            files += [ROOT / path for path in ["c/core/data_value.c", "c/core/data_stream.c",
                      "c/core/data_wire.c", "c/core/data_wire_decode.c", "c/module/blake3.c"]]
            command = [os.environ.get("CC", "cc"), "-std=gnu11", "-D_GNU_SOURCE", "-O1", "-g",
                       "-I" + str(ROOT / "c"), *map(str, files), "-o", binary]
            if os.environ.get("SANITIZE") == "1":
                command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
            built = subprocess.run(command, capture_output=True, timeout=30)
            self.assertEqual(built.returncode, 0, built.stderr)
            result = subprocess.run([binary], capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            data = json.loads(result.stdout)
            self.assertEqual(data["status"], "partial")
            self.assertEqual(data["counts"]["partial"], 1)
            self.assertEqual(path_bytes(data["items"][0]["source"]), b"raw-\xff")
            self.assertEqual(data["items"][0]["bytes_written"], str(2**64 - 1))


class MetadataResults(unittest.TestCase):
    setUp = Mutations.setUp
    runj = Mutations.runj
    manifest = Mutations.manifest
    check_report = Mutations.check_report

    def test_touch_reference_selective_and_no_create(self):
        ref, dst = self.work / "--json", self.work / "dst"
        ref.write_text("ref")
        dst.write_text("keep")
        ns = 1_700_000_000_123_456_789
        os.utime(ref, ns=(ns, ns + 100))
        os.utime(dst, ns=(ns - 10, ns - 20))
        data = self.manifest("touch", "--json", "-a", "-r", "--json", "dst")
        self.assertEqual(data["items"][0]["status"], "completed")
        self.assertEqual(dst.stat().st_atime_ns, ns)
        self.assertEqual(dst.stat().st_mtime_ns, ns - 20)
        self.assertEqual(dst.read_text(), "keep")
        data = self.manifest("touch", "--json", "-c", "absent")
        self.assertEqual(data["items"][0]["status"], "skipped")
        self.assertFalse((self.work / "absent").exists())

    def test_touch_preview_create_and_failure(self):
        data = self.manifest("touch", "--json", "--dry-run", "new")
        self.assertEqual(data["status"], "planned")
        self.assertFalse((self.work / "new").exists())
        data = self.manifest("touch", "--json", "new", "missing/child", rc=1)
        self.assertEqual(data["status"], "partial")
        self.assertTrue((self.work / "new").exists())

    def test_touch_symlink_and_fifo(self):
        (self.work / "link").symlink_to("absent")
        self.manifest("touch", "--json", "-h", "-d", "@100", "link")
        self.assertEqual((self.work / "link").lstat().st_mtime_ns, 100_000_000_000)
        self.assertFalse((self.work / "absent").exists())
        os.mkfifo(self.work / "fifo")
        self.manifest("touch", "--json", "-d", "@200", "fifo")
        self.assertEqual((self.work / "fifo").stat().st_mtime_ns, 200_000_000_000)

    def test_mkdir_existing_entry_is_not_success(self):
        (self.work / "file").write_text("keep")
        (self.work / "dir").mkdir()
        for args in (("mkdir", "dir"), ("mkdir", "-p", "file")):
            result = self.runj(shlex.join(args))
            self.assertNotEqual(result.returncode, 0, (args, result.stdout))

    def test_mkdir_parents_preview_and_modes(self):
        data = self.manifest("mkdir", "--json", "-p", "--dry-run", "a/b")
        self.assertEqual(data["status"], "planned")
        self.assertFalse((self.work / "a").exists())
        result = self.runj("umask 077; mkdir --json -p -m 0751 a/b")
        self.assertEqual(result.returncode, 0, result.stderr)
        data = json.loads(result.stdout)
        self.check_report(data, 0)
        self.assertEqual((self.work / "a/b").stat().st_mode & 0o7777, 0o751)
        self.assertEqual((self.work / "a").stat().st_mode & 0o777, 0o700)
        data = self.manifest("mkdir", "--json", "-p", "a/b")
        self.assertTrue(all(i["status"] == "skipped" for i in data["items"]))

    def test_mkdir_zero_mode_does_not_require_reopening(self):
        result = self.runj("umask 077; mkdir --json -m 000 locked")
        self.addCleanup(lambda: (self.work / "locked").exists() and (self.work / "locked").chmod(0o700))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.work / "locked").stat().st_mode & 0o7777, 0)

    def test_chmod_sequential_symbolic_clauses(self):
        for mode in ("u+x,g=u", "a-x,a+X", "u=r,g=u,o=g"):
            (self.work / "src").write_text("data")
            (self.work / "ref").write_text("data")
            (self.work / "src").chmod(0o644)
            (self.work / "ref").chmod(0o644)
            subprocess.run(["/bin/chmod", mode, str(self.work / "ref")], check=True)
            self.manifest("chmod", "--json", mode, "src")
            self.assertEqual((self.work / "src").stat().st_mode, (self.work / "ref").stat().st_mode, mode)

    def test_chmod_negative_mode_ends_option_prefix(self):
        path = self.work / "--json"
        path.write_text("data")
        path.chmod(0o666)
        result = self.runj("chmod -w --json")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, b"")
        self.assertFalse(path.stat().st_mode & 0o200)
        self.manifest("chmod", "--json", "-w", "--json")

    def test_metadata_exact_argv_and_option_preflight(self):
        names = ["a'quote", 'a"quote', "space name", os.fsdecode(b"raw-\xff"), "line\nbreak"]
        for name in names:
            data = self.manifest("mkdir", "--json", name)
            self.assertEqual(path_bytes(data["items"][0]["destination"]), os.fsencode(name))
            self.assertTrue((self.work / name).is_dir())
            self.manifest("rmdir", "--json", name)
            self.assertFalse((self.work / name).exists())
        self.manifest("mkdir", "--json", "-m", "999", "marker", rc=2)
        self.assertFalse((self.work / "marker").exists())
        self.manifest("mkdir", "--json", "--", "--json")
        self.manifest("rmdir", "--json", "--", "--json")

    def test_rmdir_preview_checks_nonempty_and_mixed_results(self):
        (self.work / "empty").mkdir()
        (self.work / "full").mkdir()
        (self.work / "full/kept").write_text("keep")
        data = self.manifest("rmdir", "--json", "--dry-run", "empty", "full", rc=1)
        self.assertEqual([i["status"] for i in data["items"]], ["planned", "failed"])
        self.assertTrue((self.work / "empty").exists())
        data = self.manifest("rmdir", "--json", "empty", "full", rc=1)
        self.assertEqual(data["status"], "partial")
        self.assertFalse((self.work / "empty").exists())
        self.assertEqual((self.work / "full/kept").read_text(), "keep")

    def test_links_collision_symlink_bytes_and_extra_operand(self):
        (self.work / "src").write_text("data")
        data = self.manifest("ln", "--json", "src", "hard")
        self.assertEqual(data["items"][0]["action"], "hardlink")
        self.assertEqual((self.work / "hard").stat().st_ino, (self.work / "src").stat().st_ino)
        self.manifest("ln", "--json", "src", "hard", rc=1)
        target = os.fsdecode(b"missing-\xff")
        self.manifest("ln", "--json", "-s", target, "sym")
        self.assertEqual(os.fsencode(os.readlink(self.work / "sym")), os.fsencode(target))
        self.manifest("ln", "--json", "-s", "src", "unexpected", "extra", rc=2)
        self.assertFalse((self.work / "unexpected").exists())

    def test_chmod_recursive_symlinks_preview_and_missing(self):
        (self.work / "tree").mkdir()
        (self.work / "tree/file").write_text("data")
        (self.work / "outside").mkdir()
        (self.work / "outside/file").write_text("keep")
        (self.work / "outside/file").chmod(0o640)
        (self.work / "tree/link").symlink_to("../outside")
        before = (self.work / "tree/file").stat().st_mode
        self.manifest("chmod", "--json", "--dry-run", "-R", "700", "tree")
        self.assertEqual((self.work / "tree/file").stat().st_mode, before)
        data = self.manifest("chmod", "--json", "-R", "700", "tree", "missing", rc=1)
        self.assertEqual(data["status"], "partial")
        self.assertEqual((self.work / "tree/file").stat().st_mode & 0o777, 0o700)
        self.assertEqual((self.work / "outside/file").stat().st_mode & 0o777, 0o640)
        self.assertTrue(any(i["status"] == "skipped" for i in data["items"]))

    def test_chmod_recursive_reports_depth_exhaustion(self):
        current = self.work / "tree"
        current.mkdir()
        for _ in range(65):
            current = current / "d"
            current.mkdir(mode=0o700)
        data = self.manifest("chmod", "--json", "-R", "0755", "tree", rc=1)
        self.assertEqual(data["status"], "partial")
        self.assertTrue(any(i["code"] == "depth_limit" for i in data["items"]))
        self.assertEqual(current.stat().st_mode & 0o777, 0o700)

    def test_metadata_output_routes_and_budget(self):
        self.env["JASH_LLM_BUDGET"] = "64"
        (self.work / "src").write_text("data")
        for code in ("touch --json src", "mkdir --json route", "rmdir --json route",
                     "chmod --json 0640 src", f"chown --json {os.getuid()} src", "ln --json src hard"):
            for route in ("{cmd} > report", "{cmd} | /bin/cat > report",
                          'r=$({cmd}); printf "%s\\n" "$r" > report'):
                if code.startswith("rmdir"): (self.work / "route").mkdir(exist_ok=True)
                if code.startswith("mkdir") and (self.work / "route").exists(): (self.work / "route").rmdir()
                if (self.work / "hard").exists(): (self.work / "hard").unlink()
                result = self.runj(route.format(cmd=code))
                self.assertEqual(result.returncode, 0, (code, result.stderr))
                self.check_report(json.loads((self.work / "report").read_bytes()), 0)

    def test_chown_numeric_validation_and_own_ids(self):
        (self.work / "src").write_text("data")
        for owner in ("4294967296", "99999999999999999999999999", "-1", "0:4294967296"):
            data = self.manifest("chown", "--json", "--", owner, "src", rc=2)
            self.assertEqual(data["items"], [])
        owner = f"{os.getuid()}:{os.getgid()}"
        self.manifest("chown", "--json", owner, "src")
        self.assertEqual((self.work / "src").stat().st_uid, os.getuid())
        self.assertEqual((self.work / "src").stat().st_gid, os.getgid())

    def test_metadata_failed_sink_and_preview_preserve_targets(self):
        (self.work / "file").write_text("keep")
        (self.work / "dir").mkdir()
        before = (self.work / "file").stat()
        commands = ["touch file", "mkdir new", "rmdir dir", "ln file new", "chmod 700 file",
                    f"chown {os.getuid()}:{os.getgid()} file"]
        for command in commands:
            op, rest = command.split(" ", 1)
            result = self.runj(f"{op} --json {rest} > /dev/full")
            self.assertEqual(result.returncode, 125, (command, result.stderr))
            self.assertFalse((self.work / "new").exists())
            self.assertTrue((self.work / "dir").exists())
            self.assertEqual((self.work / "file").stat().st_mtime_ns, before.st_mtime_ns)
            self.assertEqual((self.work / "file").stat().st_mode, before.st_mode)
            self.manifest(op, "--json", "--dry-run", *shlex.split(rest))
            self.assertFalse((self.work / "new").exists())
            self.assertTrue((self.work / "dir").exists())
            self.assertEqual((self.work / "file").stat().st_mtime_ns, before.st_mtime_ns)


class InPlaceEdit(unittest.TestCase):
    setUp = Mutations.setUp
    runj = Mutations.runj
    manifest = Mutations.manifest
    check_report = Mutations.check_report

    def test_sed_json_replacement_and_partial_result(self):
        path = self.work / "quote' name"
        path.write_text("old\nold\n")
        data = self.manifest("sed", "--json", "-i", "s/old/new/g", "missing", path.name, rc=1)
        self.assertEqual(data["status"], "partial")
        self.assertEqual([i["status"] for i in data["items"]], ["failed", "completed"])
        self.assertEqual(path.read_text(), "new\nnew\n")

    def test_sed_preview_does_not_create_backup_or_temp(self):
        (self.work / "src").write_text("old\n")
        self.env["JASH_TIMELINE"] = "1"
        data = self.manifest("sed", "--json", "--dry-run", "-i", "s/old/new/", "src")
        self.assertEqual(data["status"], "planned")
        self.assertEqual((self.work / "src").read_text(), "old\n")
        self.assertEqual([p.name for p in self.work.iterdir()], ["src"])
        self.assertFalse((self.home / ".jash/backups").exists())

    def test_sed_json_unsupported_forms_refuse_before_effect(self):
        (self.work / "src").write_text("old\n")
        for args in (("-i", "1d", "src"), ("-i.bak", "s/old/new/", "src"),
                     ("s/old/new/", "src"), ("-i", "-e", "s/old/new/", "-e", "s/new/bad/", "src")):
            data = self.manifest("sed", "--json", *args, rc=2)
            self.assertEqual(data["items"], [])
            self.assertEqual((self.work / "src").read_text(), "old\n")

    def test_sed_sink_failure_precedes_edit(self):
        (self.work / "src").write_text("old\n")
        result = self.runj("sed --json -i s/old/new/ src > /dev/full")
        self.assertEqual(result.returncode, 125, result.stderr)
        self.assertEqual((self.work / "src").read_text(), "old\n")

    def test_sed_binary_input_is_not_silently_shortened(self):
        original = b"old\x00retained\n"
        (self.work / "src").write_bytes(original)
        data = self.manifest("sed", "--json", "-i", "s/old/new/", "src", rc=1)
        self.assertEqual(data["items"][0]["code"], "binary_input_unsupported")
        self.assertEqual((self.work / "src").read_bytes(), original)

    def test_sed_all_operands_beyond_old_256_file_limit(self):
        names = [f"f{i}" for i in range(260)]
        for name in names: (self.work / name).write_text("old\n")
        data = self.manifest("sed", "--json", "-i", "s/old/new/", *names)
        self.assertEqual(len(data["items"]), len(names))
        for name in names: self.assertEqual((self.work / name).read_text(), "new\n")

    def test_sed_arbitrary_byte_paths(self):
        name = os.fsdecode(b"raw-\xff")
        (self.work / name).write_text("old\n")
        data = self.manifest("sed", "--json", "-i", "s/old/new/", name)
        self.assertEqual(path_bytes(data["items"][0]["source"]), os.fsencode(name))
        self.assertEqual((self.work / name).read_text(), "new\n")


class MetadataFaults(unittest.TestCase):
    setUp = Mutations.setUp
    runj = Mutations.runj
    manifest = Mutations.manifest
    check_report = Mutations.check_report

    @classmethod
    def setUpClass(cls):
        tmp = tempfile.TemporaryDirectory(prefix="jash-metadata-faults-")
        cls.addClassCleanup(tmp.cleanup)
        cls.hook = str(Path(tmp.name) / "faults.so")
        subprocess.run([os.environ.get("CC", "cc"), "-shared", "-fPIC",
                        str(ROOT / "tests/mutation_metadata_faults.c"), "-ldl", "-o", cls.hook],
                       check=True, capture_output=True, timeout=30)

    def test_touch_created_file_survives_timestamp_failure(self):
        self.env.update(LD_PRELOAD=self.hook, JASH_TEST_MUTATION_FAULT="touch_times",
                        JASH_TEST_MUTATION_PATH=str(self.work / "new"))
        data = self.manifest("touch", "--json", "new", rc=1)
        self.assertTrue((self.work / "new").exists())
        self.assertEqual(data["status"], "partial")
        self.assertEqual(data["items"][0]["status"], "partial")

    def test_cancel_after_first_effect_stops_remaining_operands(self):
        self.env.update(LD_PRELOAD=self.hook, JASH_TEST_MUTATION_FAULT="cancel_after_first")
        for operation in ("rm", "mv"):
            (self.work / "first").write_text("first")
            (self.work / "second").write_text("keep")
            (self.work / "dest").mkdir(exist_ok=True)
            args = [operation, "--json", "first", "second"]
            if operation == "mv": args.append("dest")
            result = self.runj(shlex.join(args))
            self.assertEqual(result.returncode, 143, (operation, result.stderr))
            report = json.loads(result.stdout)
            self.assertEqual(report["status"], "cancelled")
            self.assertEqual(report["exit_code"], 143)
            self.assertEqual(len(report["items"]), 1)
            self.assertEqual(report["items"][0]["status"], "completed")
            self.assertFalse((self.work / "first").exists())
            self.assertEqual((self.work / "second").read_text(), "keep")

    def test_sed_rollback_failure_path_is_not_reported_as_no_effect(self):
        (self.work / "src").write_text("old\n")
        self.env.update(LD_PRELOAD=self.hook, JASH_TEST_MUTATION_FAULT="sed_parent_sync",
                        JASH_TEST_MUTATION_PATH=str(self.work))
        data = self.manifest("sed", "--json", "-i", "s/old/new/", "src", rc=1)
        self.assertEqual(data["status"], "partial")
        self.assertEqual(data["items"][0]["code"], "publication_sync")
        self.assertEqual(data["items"][0]["effect"], "possible")
        self.assertEqual((self.work / "src").read_text(), "old\n")


if __name__ == "__main__":
    unittest.main()
