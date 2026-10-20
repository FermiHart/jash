#!/usr/bin/env python3
"""Miniature trusted registries only; never execute the master gate list."""
import contextlib
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import shlex
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
RUNNER = ROOT / "tools/suite_runner.py"
MASTER = ROOT / "tests/test_suite.sh"
RUNNER_SPEC = importlib.util.spec_from_file_location("suite_runner_pidfd", RUNNER)
RUNNER_MODULE = importlib.util.module_from_spec(RUNNER_SPEC)
RUNNER_SPEC.loader.exec_module(RUNNER_MODULE)
EXPORT = """#!/usr/bin/env bash
set -e
[[ ${1-} == --suite-export-v1 ]] || exit 90
run() { builtin printf '%%s\\0' "$#" "$@"; }
registrations() {
%s
}
registrations
"""


def registration(name, *argv):
    return "run " + " ".join(shlex.quote(s) for s in (name, *argv))


class SuiteRunnerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="suite-runner-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / "tests").mkdir()
        self.registry = self.root / "tests/test_suite.sh"
        self.output = self.root / "output"
        self.marker = self.root / "gate-ran"
        self.env = dict(os.environ, JASH_SUITE_PINNED="1",
                        JASH_MODULE_DEV_ROOT="/not-the-private-snapshot",
                        JASH_SIGIL="2", JASH_GUARD_BYPASS="1",
                        JASH_GUARD_BYPASS_STRICT="1", TMPDIR=str(self.root))

    def fixture(self, *lines):
        self.registry.write_text(EXPORT % "\n".join(lines))

    def gate(self, code, name="gate"):
        return registration(name, sys.executable, "-c", code)

    def command(self, *args):
        return [sys.executable, str(RUNNER), "--registry", str(self.registry), *args]

    def invoke(self, *args):
        return subprocess.run(self.command(*args), cwd=self.root, env=self.env,
                              capture_output=True, timeout=15)

    def execute(self, *args):
        return self.invoke("--output", str(self.output), *args)

    def events(self):
        return [json.loads(line) for line in
                (self.output / "events.jsonl").read_text().splitlines()]

    def assert_complete(self):
        marker = json.loads((self.output / "COMPLETE").read_text())
        manifest = json.loads((self.output / "manifest.json").read_text())
        self.assertEqual(marker["version"], 1)
        self.assertEqual(marker["status"], "committed")
        self.assertEqual(marker["manifest_sha256"], manifest["manifest_sha256"])
        self.assertEqual(marker["events_sha256"],
                         hashlib.sha256((self.output / "events.jsonl").read_bytes()).hexdigest())
        self.assertFalse((self.output / "INCOMPLETE").exists())

    def wait_for(self, predicate, process=None):
        end = time.monotonic() + 8
        while time.monotonic() < end:
            if predicate():
                return
            if process is not None and process.poll() is not None:
                stdout, stderr = process.communicate()
                self.fail(f"runner exited before readiness rc={process.returncode} "
                          f"stdout={stdout[-2048:]!r} stderr={stderr[-2048:]!r}")
            time.sleep(.02)
        self.fail("readiness deadline exceeded")

    def track_pid(self, pid):
        # Failure cleanup must not signal a PID recycled after the assertion.
        fd = RUNNER_MODULE.pidfd_open(pid)
        def cleanup():
            try:
                RUNNER_MODULE.pidfd_send_signal(fd, signal.SIGKILL)
            except ProcessLookupError:
                pass
            finally:
                os.close(fd)
        self.addCleanup(cleanup)

    def test_list_fidelity_duplicates_and_no_effects(self):
        argv = ["bash", "-c", "printf '%s\\n' \"a b\"\n# literal newline", "", "*", "a'b"]
        label = "multilingual: \u65e5\u672c\u8a9e / \u00e7 ../escape"
        self.fixture(registration(label, *argv), registration(label, *argv))
        before = set(self.root.iterdir())
        result = self.invoke("--list")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual([json.loads(s) for s in result.stdout.splitlines()],
                         [dict(index=i, name=label, argv=argv) for i in (1, 2)])
        self.assertEqual(set(self.root.iterdir()), before)
        result = self.invoke("--list", "--from", "2", "--to", "2")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout), dict(index=2, name=label, argv=argv))
        self.assertEqual(set(self.root.iterdir()), before)

    def test_execution_preserves_literal_argv(self):
        arguments = ["", "a b", "a'b", '"quoted"', "*", "x\ny", "\u65e5\u672c\u8a9e", "$(touch forbidden)"]
        self.fixture(registration("literal argv", sys.executable, "-c",
                                  "import json,sys; print(json.dumps(sys.argv[1:]))", *arguments))
        result = self.execute()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads((self.output / "0001.stdout").read_bytes()), arguments)

    def test_bash_startup_hooks_are_not_inherited(self):
        hook_marker = self.root / "hook-ran"
        gate_marker = self.root / "gate-ran"
        hook = self.root / "bash-env"
        hook.write_text(f"touch {shlex.quote(str(hook_marker))}\nexit 0\n")
        self.fixture(registration("bash gate", "bash", "-c",
                                  f"touch {shlex.quote(str(gate_marker))}"))
        env = dict(self.env, BASH_ENV=str(hook), ENV=str(hook))
        result = subprocess.run(self.command("--output", str(self.output)), cwd=self.root,
                                env=env, capture_output=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(gate_marker.exists())
        self.assertFalse(hook_marker.exists())

    def test_invalid_arguments_have_no_effects(self):
        self.fixture(self.gate(f"open({str(self.marker)!r}, 'w').close()"))
        for args in [("--unknown",), ("--from", "2", "--to", "1"),
                     ("--from", "0"), ("--to", "2"), ("--from", "2"),
                     ("--timeout", "0"), ("--timeout", "-1"),
                     ("--timeout", "nan"), ("--timeout", "inf"),
                     ("--timeout", "1e999"), ("--from", "bad"),
                     ("--list", "--to", "99"), ("--time", "1")]:
            with self.subTest(args=args):
                before = set(self.root.iterdir())
                result = self.execute(*args)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(set(self.root.iterdir()), before)
        self.fixture()
        self.assertNotEqual(self.execute().returncode, 0)
        self.assertFalse(self.output.exists())

    def test_range_manifest_and_numeric_logs(self):
        self.fixture(self.gate("raise SystemExit(1)"),
                     self.gate("print('selected')", "../../escape"),
                     self.gate("raise SystemExit(1)"))
        source = self.registry.read_bytes()
        listed = [json.loads(line) for line in self.invoke("--list").stdout.splitlines()]
        result = self.execute("--from", "2", "--to", "2")
        self.assertEqual(result.returncode, 0, result.stderr)
        manifest = json.loads((self.output / "manifest.json").read_text())
        digest = manifest.pop("manifest_sha256")
        canonical = json.dumps(manifest, sort_keys=True, separators=(",", ":"),
                               ensure_ascii=True).encode()
        self.assertEqual(digest, hashlib.sha256(canonical).hexdigest())
        self.assertEqual(manifest["source"]["sha256"], hashlib.sha256(source).hexdigest())
        self.assertEqual(len(manifest["gates"]), 3)
        self.assertEqual(manifest["gates"], listed)
        self.assertEqual(manifest["selected"], [2])
        self.assertEqual(self.output.stat().st_mode & 0o777, 0o700)
        self.assert_complete()
        events = self.events()
        self.assertEqual([e["event"] for e in events], ["start", "result", "summary"])
        gate = events[1]
        self.assertEqual(gate["stdout"], "0002.stdout")
        self.assertEqual((self.output / gate["stdout"]).read_bytes(), b"selected\n")
        self.assertFalse((self.root / "escape").exists())
        summary = events[-1]
        self.assertEqual([summary[k] for k in ("registered", "selected", "completed", "unselected")],
                         [3, 1, 1, 2])
        self.assertTrue(summary["selected_exited_zero"])
        self.assertFalse(summary["all_registered_exited_zero"])

    def test_status_matrix(self):
        self.fixture(*(self.gate(f"raise SystemExit({rc})", str(rc)) for rc in (0, 1, 77, 124)),
                     registration("missing", str(self.root / "no-executable")),
                     self.gate("import os, signal; os.kill(os.getpid(), signal.SIGUSR1)", "signal"),
                     self.gate("import time; time.sleep(30)", "timeout"))
        result = self.execute("--timeout", "2")
        self.assertEqual(result.returncode, 1, result.stderr)
        results = [e for e in self.events() if e["event"] == "result"]
        self.assertEqual([e["status"] for e in results],
                         ["exited_zero", "failed", "skipped", "failed", "spawn_error", "signaled", "timed_out"])
        self.assertEqual([e["returncode"] for e in results[:5]], [0, 1, 77, 124, None])
        self.assertFalse(results[3]["timeout"])
        self.assertEqual(results[5]["signal"], signal.SIGUSR1)
        self.assertTrue(results[6]["timeout"])
        self.assertGreater(results[6]["duration"], 1.9)
        summary = self.events()[-1]
        self.assertEqual([summary[k] for k in ("completed", "failed", "skipped", "timed_out")], [7, 5, 1, 1])

    def test_gate_sigterm_is_failure_not_runner_interruption(self):
        self.fixture(registration("self signal", "bash", "-c", "kill -TERM $$"),
                     self.gate(f"open({str(self.marker)!r}, 'w').close()", "after signal"))
        result = self.execute()
        self.assertEqual(result.returncode, 1, result.stderr)
        results = [event for event in self.events() if event["event"] == "result"]
        self.assertEqual([event["status"] for event in results], ["signaled", "exited_zero"])
        self.assertTrue(self.marker.exists())
        self.assertEqual(self.events()[-1]["interrupted"], 0)

    def test_skip_is_nonzero_and_legacy_skip_is_not_coverage(self):
        self.fixture(self.gate("raise SystemExit(77)"))
        self.assertEqual(self.execute().returncode, 1)
        self.output = self.root / "legacy-output"
        self.fixture(self.gate("print('SKIP arbitrary words')"))
        self.assertEqual(self.execute().returncode, 0)
        summary = self.events()[-1]
        self.assertEqual(summary["skipped"], 0)
        self.assertTrue(summary["selected_exited_zero"])
        self.assertTrue(summary["limitations"])

    def test_exit_zero_with_live_residual_child_is_failure(self):
        self.fixture(registration("leaked background work", "bash", "-c",
                                  "sleep 60 & echo $!; exit 0"))
        result = self.execute()
        child = int((self.output / "0001.stdout").read_bytes())
        self.assertFalse(Path(f"/proc/{child}").exists(), "owned background child must be reaped")
        self.assertEqual(result.returncode, 1, result.stderr)
        gate, summary = self.events()[1:]
        self.assertEqual(gate["status"], "residual_processes")
        self.assertEqual(gate["returncode"], 0, "preserve the observed gate-leader exit code")
        self.assertIsNone(gate["signal"])
        self.assertFalse(gate["timeout"])
        self.assertGreater(gate["signaled_descendants"], 0)
        self.assertIsNone(gate["cleanup_error"])
        self.assertEqual(summary["failed"], 1)
        self.assertEqual(summary["residual_processes"], 1)
        self.assertFalse(summary["selected_exited_zero"])
        self.assertFalse(summary["all_registered_exited_zero"])

    def test_exit_zero_with_adopted_zombie_is_success(self):
        # WNOWAIT proves the child is dead but leaves it for the subreaper.
        self.fixture(self.gate("import os\n"
                               "child = os.fork()\n"
                               "if child == 0: os._exit(0)\n"
                               "os.waitid(os.P_PID, child, os.WEXITED | os.WNOWAIT)\n"
                               "os.write(1, str(child).encode())\n"
                               "os._exit(0)\n"))
        result = self.execute()
        child = int((self.output / "0001.stdout").read_bytes())
        self.assertFalse(Path(f"/proc/{child}").exists(), "adopted zombie must be reaped")
        self.assertEqual(result.returncode, 0, result.stderr)
        gate, summary = self.events()[1:]
        self.assertEqual(gate["signaled_descendants"], 0, "zombies must not count as live signal targets")
        self.assertEqual(gate["status"], "exited_zero")
        self.assertEqual(gate["returncode"], 0)
        self.assertIsNone(gate["cleanup_error"])
        self.assertEqual(summary["failed"], 0)
        self.assertEqual(summary["residual_processes"], 0)
        self.assertTrue(summary["selected_exited_zero"])
        self.assertTrue(summary["all_registered_exited_zero"])

    def test_adopted_zombie_is_reaped_while_gate_is_active(self):
        code = ("import errno,os,time\n"
                "leader=os.fork()\n"
                "if leader == 0:\n"
                " os.setpgid(0,0)\n"
                " if os.fork() == 0: os._exit(0)\n"
                " os._exit(0)\n"
                "os.waitpid(leader,0)\n"
                "deadline=time.monotonic()+5\n"
                "while time.monotonic() < deadline:\n"
                " try: os.kill(-leader,0)\n"
                " except ProcessLookupError: raise SystemExit(0)\n"
                " time.sleep(.02)\n"
                "raise SystemExit(1)\n")
        self.fixture(self.gate(code, "adopted zombie must not perturb gate"))
        result = self.execute()
        self.assertEqual(result.returncode, 0, result.stderr)
        gate, summary = self.events()[1:]
        self.assertEqual(gate["status"], "exited_zero")
        self.assertEqual(gate["signaled_descendants"], 0)
        self.assertTrue(summary["selected_exited_zero"])

    def test_registry_mutation_is_pinned(self):
        self.fixture(self.gate(f"from pathlib import Path; Path({str(self.registry)!r}).write_text('exit 42\\n')"),
                     self.gate("print('second gate from captured bytes')"))
        original = self.registry.read_bytes()
        result = self.execute()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.output / "registry.sh").read_bytes(), original)
        self.assertEqual((self.output / "0002.stdout").read_bytes(), b"second gate from captured bytes\n")
        self.assertEqual(self.events()[-1]["completed"], 2)

    def test_environment_and_private_module_snapshot(self):
        module = self.root / "modules/sample/sample.modjash"
        module.parent.mkdir(parents=True)
        module.write_bytes(b"module bytes")
        self.fixture(self.gate("import os,json; print(json.dumps(dict(os.environ),sort_keys=True)); print(os.getcwd())"))
        result = self.execute()
        self.assertEqual(result.returncode, 0, result.stderr)
        lines = (self.output / "0001.stdout").read_text().splitlines()
        env = json.loads(lines[0])
        for key in ("HOME", "PATH"):
            self.assertEqual(env.get(key), self.env.get(key))
        for key in ("JASH_SIGIL", "JASH_GUARD_BYPASS", "JASH_GUARD_BYPASS_STRICT"):
            self.assertEqual(env[key], "0")
        self.assertNotIn("JASH_SUITE_PINNED", env)
        self.assertEqual(lines[1], str(self.root))
        self.assertEqual(env["JASH_MODULE_DEV_ROOT"], str(self.output / "modules"))
        self.assertEqual(env["TMPDIR"], str(self.output / "tmp"))
        copy = self.output / "modules/sample/sample.modjash"
        self.assertEqual(copy.read_bytes(), b"module bytes")
        self.assertNotEqual(copy.stat().st_ino, module.stat().st_ino)
        self.assertEqual(copy.stat().st_mode & 0o777, 0o600)

    def test_outside_invocation_uses_registry_root_not_decoy(self):
        caller = self.root / "caller"
        (caller / "tests").mkdir(parents=True)
        (self.root / "tests/which.sh").write_text("printf 'real gate\\n'; pwd\n")
        (caller / "tests/which.sh").write_text("printf 'decoy gate\\n'; pwd\n")
        self.fixture(registration("relative gate", "bash", "tests/which.sh"))
        self.output = caller / "relative-output"
        result = subprocess.run(["bash", str(MASTER), "--registry", str(self.registry),
                                 "--output", "relative-output"], cwd=caller, env=self.env,
                                capture_output=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.output / "0001.stdout").read_text(), f"real gate\n{self.root}\n")
        manifest = json.loads((self.output / "manifest.json").read_text())
        self.assertEqual(manifest["cwd"], str(self.root))
        self.assertEqual(manifest["invocation_cwd"], str(caller))
        self.assertFalse((self.root / "relative-output").exists())

    def test_existing_output_and_symlink_are_refused(self):
        self.fixture(self.gate(f"open({str(self.marker)!r}, 'w').close()"))
        self.output.symlink_to(self.root, target_is_directory=True)
        self.assertNotEqual(self.execute().returncode, 0)
        self.assertFalse(self.marker.exists())
        self.output.unlink()
        self.output.mkdir()
        self.assertNotEqual(self.execute().returncode, 0)
        self.assertFalse(self.marker.exists())

    def test_output_swap_during_creation_cannot_redirect_artifacts(self):
        spec = importlib.util.spec_from_file_location("suite_runner_output_swap", RUNNER)
        runner = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(runner)
        victim = self.root / "victim"
        displaced = self.root / "displaced-output"
        victim.mkdir()
        self.fixture(self.gate(f"open({str(self.marker)!r}, 'w').close()"))
        original_mkdir = os.mkdir
        swapped = False
        def swap_after_create(path, mode=0o777, *, dir_fd=None):
            nonlocal swapped
            if dir_fd is None:
                result = original_mkdir(path, mode)
                created = Path(path)
            else:
                result = original_mkdir(path, mode, dir_fd=dir_fd)
                created = Path(os.readlink(f"/proc/self/fd/{dir_fd}")) / os.fsdecode(path)
            if not swapped and created == self.output:
                created.rename(displaced)
                created.symlink_to(victim, target_is_directory=True)
                swapped = True
            return result
        with mock.patch.object(runner.os, "mkdir", side_effect=swap_after_create), \
                contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            rc = runner.main(self.command()[2:] + ["--output", str(self.output)])
        self.assertNotEqual(rc, 0)
        self.assertFalse(self.marker.exists())
        self.assertEqual(list(victim.iterdir()), [])

    def test_output_swap_while_gate_runs_keeps_writes_on_pinned_directory(self):
        release = self.root / "release"
        displaced = self.root / "displaced-output"
        victim = self.root / "victim"
        victim.mkdir()
        self.fixture(self.gate("import time\nfrom pathlib import Path\n"
                               f"while not Path({str(release)!r}).exists(): time.sleep(.02)\n"
                               "print('pinned log')\n"))
        process = subprocess.Popen(self.command("--output", str(self.output)), cwd=self.root,
                                   env=self.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            self.wait_for(lambda: (self.output / "events.jsonl").exists()
                          and b'"event":"start"' in (self.output / "events.jsonl").read_bytes(),
                          process)
            self.output.rename(displaced)
            self.output.symlink_to(victim, target_is_directory=True)
            release.touch()
            out, err = process.communicate(timeout=10)
            self.assertEqual(process.returncode, 1, (out, err))
        finally:
            if process.poll() is None:
                process.terminate()
                process.communicate(timeout=5)
        self.assertEqual((displaced / "0001.stdout").read_text(), "pinned log\n")
        self.assertFalse((victim / "manifest.json").exists())
        summary = json.loads((displaced / "events.jsonl").read_text().splitlines()[-1])
        self.assertEqual(summary["status"], "runner_error")
        self.assertIn("no longer names", summary["runner_error"])
        self.output = displaced
        self.assert_complete()

    def test_default_output_is_private(self):
        self.fixture(self.gate("pass"))
        result = self.invoke()
        self.assertEqual(result.returncode, 0, result.stderr)
        outputs = list(self.root.glob("jash-suite-*/manifest.json"))
        self.assertEqual(len(outputs), 1)
        self.assertEqual(outputs[0].parent.stat().st_mode & 0o777, 0o700)

    def test_binary_large_output_is_spooled(self):
        self.fixture(self.gate("import os; data=bytes(range(256))*4096; "
                               "[os.write(1,data) for _ in range(8)]; os.write(2,b'\\xff\\x00err')"))
        result = self.execute()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertLess(len(result.stdout) + len(result.stderr), 8192)
        self.assertEqual((self.output / "0001.stdout").stat().st_size, 8 * 1024 * 1024)
        self.assertEqual((self.output / "0001.stderr").read_bytes(), b"\xff\x00err")

    def test_progress_is_flushed_concise_and_json_escaped(self):
        label = '\x1b[31mline\n\rc1\x85\u2028"' + "x" * 1000
        release_first = self.root / "release-first"
        release_second = self.root / "release-second"
        payload = b"\x00\xffraw gate output\n"
        self.fixture(*(self.gate("import os,time\nfrom pathlib import Path\n"
                                 f"os.write(1, {payload!r})\n"
                                 f"while not Path({str(release)!r}).exists(): time.sleep(.02)\n", label)
                       for release in (release_first, release_second)))
        console = self.root / "console"
        with console.open("wb") as stream:
            process = subprocess.Popen(self.command("--output", str(self.output)),
                                       cwd=self.root, env=self.env, stdout=stream, stderr=subprocess.PIPE)
            try:
                self.wait_for(lambda: b"suite start " in console.read_bytes(), process)
                self.assertIsNone(process.poll(), "start must be visible before gate completion")
                release_first.touch()
                self.wait_for(lambda: b"suite finish " in console.read_bytes(), process)
                self.assertIsNone(process.poll(), "finish must be flushed while later gates are running")
                release_second.touch()
                _, err = process.communicate(timeout=10)
                self.assertEqual(process.returncode, 0, err)
            finally:
                if process.poll() is None:
                    process.terminate()
                    process.communicate(timeout=5)
        lines = [line for line in console.read_bytes().splitlines()
                 if line.startswith((b"suite start ", b"suite finish "))]
        self.assertEqual([line.split(b" ", 2)[:2] for line in lines],
                         [[b"suite", phase] for phase in (b"start", b"finish", b"start", b"finish")])
        for line in lines:
            self.assertLess(len(line), 1024)
            self.assertTrue(all(32 <= byte < 127 for byte in line), "escape all terminal controls")
            record = json.loads(line.split(b" ", 2)[2])
            self.assertTrue(record["name"].startswith(label[:40]))
            if line.startswith(b"suite finish "):
                self.assertEqual(record["status"], "exited_zero")
        self.assertEqual([e["name"] for e in self.events() if e["event"] == "start"], [label, label])
        for index in (1, 2):
            self.assertEqual((self.output / f"{index:04d}.stdout").read_bytes(), payload)

    def test_early_write_failure_does_not_spawn(self):
        spec = importlib.util.spec_from_file_location("suite_runner", RUNNER)
        runner = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(runner)
        self.fixture(self.gate(f"open({str(self.marker)!r}, 'w').close()"))
        original_fsync = os.fsync
        def fail_events(fd):
            if os.readlink(f"/proc/self/fd/{fd}").endswith("events.jsonl"):
                raise OSError("injected durable event write failure")
            original_fsync(fd)
        with mock.patch.object(runner.os, "fsync", side_effect=fail_events), \
                contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            self.assertNotEqual(runner.main(self.command()[2:] + ["--output", str(self.output)]), 0)
        self.assertFalse(self.marker.exists())

    def test_summary_write_failure_removes_visible_success_summary(self):
        spec = importlib.util.spec_from_file_location("suite_runner_summary_failure", RUNNER)
        runner = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(runner)
        self.fixture(self.gate("pass"))
        original_durable = runner.durable
        def fail_summary(file, data):
            original_durable(file, data)
            if b'"event":"summary"' in data:
                raise OSError("injected summary durability failure")
        with mock.patch.object(runner, "durable", side_effect=fail_summary), \
                contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            self.assertNotEqual(runner.main(self.command()[2:] + ["--output", str(self.output)]), 0)
        events = [json.loads(line) for line in
                  (self.output / "events.jsonl").read_text().splitlines()]
        self.assertNotEqual(events[-1]["event"], "summary")
        self.assertTrue((self.output / "INCOMPLETE").exists())
        self.assertFalse((self.output / "COMPLETE").exists())

    def test_commit_rename_failure_leaves_incomplete_evidence(self):
        spec = importlib.util.spec_from_file_location("suite_runner_commit_failure", RUNNER)
        runner = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(runner)
        self.fixture(self.gate("pass"))
        with mock.patch.object(runner.os, "replace", side_effect=OSError("injected commit rename failure")), \
                contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            self.assertNotEqual(runner.main(self.command()[2:] + ["--output", str(self.output)]), 0)
        self.assertTrue((self.output / "INCOMPLETE").exists())
        self.assertFalse((self.output / "COMPLETE").exists())

    def test_post_commit_directory_fsync_failure_cannot_create_false_green(self):
        spec = importlib.util.spec_from_file_location("suite_runner_commit_fsync", RUNNER)
        runner = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(runner)
        self.fixture(self.gate("pass"))
        original_fsync = os.fsync
        def fail_after_commit(fd):
            if (os.readlink(f"/proc/self/fd/{fd}") == str(self.output)
                    and not (self.output / "INCOMPLETE").exists()):
                raise OSError("injected post-commit directory fsync failure")
            original_fsync(fd)
        stderr = io.StringIO()
        with mock.patch.object(runner.os, "fsync", side_effect=fail_after_commit), \
                contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(stderr):
            self.assertEqual(runner.main(self.command()[2:] + ["--output", str(self.output)]), 0)
        self.assertIn("commit directory fsync failed", stderr.getvalue())
        self.assert_complete()

    def test_signal_after_results_does_not_retroactively_interrupt(self):
        spec = importlib.util.spec_from_file_location("suite_runner_final_signal", RUNNER)
        runner = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(runner)
        self.fixture(self.gate("pass"))
        original_durable = runner.durable
        def signal_on_summary(file, data):
            if b'"event":"summary"' in data:
                os.kill(os.getpid(), signal.SIGTERM)
            original_durable(file, data)
        with mock.patch.object(runner, "durable", side_effect=signal_on_summary), \
                contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(runner.main(self.command()[2:] + ["--output", str(self.output)]), 0)
        summary = self.events()[-1]
        self.assertEqual(summary["status"], "completed")
        self.assertTrue(summary["selected_exited_zero"])

    def test_log_directory_is_durable_before_spawn(self):
        spec = importlib.util.spec_from_file_location("suite_runner", RUNNER)
        runner = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(runner)
        self.fixture(self.gate(f"open({str(self.marker)!r}, 'w').close()"))
        original_fsync = os.fsync
        def fail_log_directory(fd):
            if (os.readlink(f"/proc/self/fd/{fd}") == str(self.output)
                    and (self.output / "0001.stdout").exists()):
                raise OSError("log directory sync failed")
            original_fsync(fd)
        with mock.patch.object(runner.os, "fsync", side_effect=fail_log_directory), \
                contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            self.assertNotEqual(runner.main(self.command()[2:] + ["--output", str(self.output)]), 0)
        self.assertFalse(self.marker.exists())

    def test_unsupported_subtree_boundary_fails_closed(self):
        spec = importlib.util.spec_from_file_location("suite_runner", RUNNER)
        runner = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(runner)
        self.fixture(self.gate(f"open({str(self.marker)!r}, 'w').close()"))
        with mock.patch.object(runner.sys, "platform", "darwin"), \
                contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            self.assertNotEqual(runner.main(self.command()[2:] + ["--output", str(self.output)]), 0)
        self.assertFalse(self.output.exists())
        self.assertFalse(self.marker.exists())

    def test_optional_children_file_absent_still_rejects_live_residual(self):
        runner = RUNNER_MODULE
        observer = runner.proc_observation
        original_read = observer._read_bounded
        absent = []
        def read(directory, path, limit):
            if path.endswith("/children"):
                absent.append(path)
                raise FileNotFoundError(2, "injected optional proc source absent", path)
            return original_read(directory, path, limit)
        self.fixture(registration("residual with optional source absent", "bash", "-c",
                                  "sleep 60 & echo $!; exit 0"))
        with mock.patch.object(observer, "_read_bounded", side_effect=read), \
                contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            observer.direct_children(os.getpid())
            rc = runner.main(self.command()[2:] + ["--output", str(self.output)])
        self.assertTrue(absent, "fixture must force the missing optional source")
        self.assertEqual(rc, 1)
        child = int((self.output / "0001.stdout").read_bytes())
        self.assertFalse(Path(f"/proc/{child}").exists())
        result, summary = self.events()[1:]
        self.assertEqual(result["status"], "residual_processes")
        self.assertGreater(result["signaled_descendants"], 0)
        self.assertIsNone(result["cleanup_error"])
        self.assertFalse(summary["selected_exited_zero"])

    def test_denied_proc_observation_prevents_gate_start(self):
        runner = RUNNER_MODULE
        self.fixture(self.gate(f"open({str(self.marker)!r}, 'w').close()"))
        diagnostic = io.StringIO()
        with mock.patch.object(runner.proc_observation, "direct_children",
                               side_effect=PermissionError(13, "injected proc denial")), \
                contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(diagnostic):
            rc = runner.main(self.command()[2:] + ["--output", str(self.output)])
        self.assertEqual(rc, 1)
        self.assertIn("injected proc denial", diagnostic.getvalue())
        self.assertFalse(self.output.exists())
        self.assertFalse(self.marker.exists())

    def test_descendant_observation_failure_never_becomes_cleanup_success(self):
        runner = RUNNER_MODULE
        process = mock.Mock(pid=42, returncode=0)
        with mock.patch.object(runner, "descendants",
                               side_effect=runner.proc_observation.ProcObservationError("incomplete source")):
            outcome = runner.cleanup_tree(process)
        self.assertEqual(outcome["signaled_descendants"], 0)
        self.assertIn("incomplete source", outcome["cleanup_error"])

    def test_empty_proc_snapshot_requires_waitid_echild_for_quiescence(self):
        runner = RUNNER_MODULE
        process = mock.Mock(pid=42, returncode=0)
        with mock.patch.object(runner, "descendants", return_value=set()), \
                mock.patch.object(runner.os, "waitid", side_effect=[None, ChildProcessError]) as waited, \
                mock.patch.object(runner.time, "sleep"):
            outcome = runner.cleanup_tree(process)
        self.assertIsNone(outcome["cleanup_error"])
        self.assertEqual(waited.call_count, 2, "a live child is not an empty complete subtree")
        with mock.patch.object(runner, "descendants", return_value=set()), \
                mock.patch.object(runner.os, "waitid", return_value=None), \
                mock.patch.object(runner.time, "monotonic", side_effect=[0, 0, 4]), \
                mock.patch.object(runner.time, "sleep"):
            outcome = runner.cleanup_tree(process)
        self.assertIn("descendants remain", outcome["cleanup_error"])

    def test_dead_pidfd_is_reopened_for_recycled_descendant_number(self):
        spec = importlib.util.spec_from_file_location("suite_runner_pid_reuse", RUNNER)
        runner = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(runner)
        handles = {42: 100}
        with mock.patch.object(runner, "pidfd_send_signal", side_effect=ProcessLookupError), \
                mock.patch.object(runner, "pidfd_open", return_value=200) as opened, \
                mock.patch.object(runner.os, "close") as closed:
            self.assertEqual(runner.pidfd_for(handles, 42), 200)
        closed.assert_called_once_with(100)
        opened.assert_called_once_with(42)
        self.assertEqual(handles, {42: 200})

    def test_registry_symlink_swap_cannot_change_execution_root(self):
        real = self.root / "real"
        decoy = self.root / "decoy"
        link = self.root / "linked"
        for directory, text in ((real, "real\n"), (decoy, "decoy\n")):
            (directory / "tests").mkdir(parents=True)
            (directory / "modules").mkdir()
            (directory / "tests/which.sh").write_text(f"printf {text!r}")
        linked_registry = link / "tests/test_suite.sh"
        source = EXPORT % (f"rm -f {shlex.quote(str(link))}\n"
                           f"ln -s {shlex.quote(str(decoy))} {shlex.quote(str(link))}\n"
                           + registration("relative", "bash", "tests/which.sh"))
        (real / "tests/test_suite.sh").write_text(source)
        (decoy / "tests/test_suite.sh").write_text(EXPORT % registration(
            "decoy", "bash", "tests/which.sh"))
        link.symlink_to(real, target_is_directory=True)
        self.registry = linked_registry
        result = self.execute()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.output / "0001.stdout").read_bytes(), b"real\n")
        manifest = json.loads((self.output / "manifest.json").read_text())
        self.assertEqual(manifest["cwd"], str(real))

    @unittest.skipUnless(sys.platform == "linux", "Linux descendant guarantee")
    def test_interrupts_cleanup_nested_sessions_and_stop_selection(self):
        for sig in (signal.SIGINT, signal.SIGTERM):
            for target_group in (False, True):
                with self.subTest(sig=sig, group=target_group):
                    self.output = self.root / f"out-{sig}-{target_group}"
                    ready = self.root / f"ready-{sig}-{target_group}"
                    nested = ("import os,signal,time; "
                              "signal.signal(signal.SIGTERM,signal.SIG_IGN); "
                              "signal.signal(signal.SIGINT,signal.SIG_IGN); "
                              f"open({str(ready)!r},'w').write(str(os.getpid())); time.sleep(60)")
                    parent = ("import os,subprocess,time; print(os.getpid(),flush=True); "
                              f"subprocess.Popen({[sys.executable, '-c', nested]!r},start_new_session=True); "
                              "time.sleep(60)")
                    self.fixture(self.gate(parent), self.gate(f"open({str(self.marker)!r}, 'w').close()"))
                    process = subprocess.Popen(self.command("--output", str(self.output)),
                                               cwd=self.root, env=self.env, stdout=subprocess.PIPE,
                                               stderr=subprocess.PIPE, start_new_session=True)
                    try:
                        self.wait_for(lambda: ready.exists() and ready.stat().st_size > 0, process)
                        child = int(ready.read_text())
                        self.track_pid(child)
                        gate = int((self.output / "0001.stdout").read_text().strip())
                        if target_group:
                            os.killpg(os.getpgid(gate), sig)
                        else:
                            process.send_signal(sig)
                        out, err = process.communicate(timeout=8)
                        self.assertEqual(process.returncode, 128 + sig, (out, err))
                        self.assertFalse(Path(f"/proc/{child}").exists())
                        self.assertFalse(self.marker.exists())
                        events = self.events()
                        self.assertEqual(events[-1]["status"], "interrupted")
                        self.assertEqual(events[-1]["completed"], 1)
                        self.assertEqual(events[-1]["interrupted"], 1)
                        self.assertEqual(events[0]["event"], "start")
                    finally:
                        if process.poll() is None:
                            process.terminate()
                            try:
                                process.communicate(timeout=5)
                            except subprocess.TimeoutExpired:
                                process.kill()
                                process.communicate()

    def test_gate_group_interrupt_when_bash_defers_signal(self):
        ready = self.root / "ready"
        nested = ("import os,signal,time; "
                  "signal.signal(signal.SIGINT,signal.SIG_IGN); "
                  "signal.signal(signal.SIGTERM,signal.SIG_IGN); "
                  f"open({str(ready)!r},'w').write(str(os.getpid())); time.sleep(60)")
        # Bash defers SIGINT while waiting for a child in another session.
        command = f"echo $$; setsid {shlex.quote(sys.executable)} -c {shlex.quote(nested)}; echo late"
        self.fixture(registration("deferred shell signal", "bash", "-c", command),
                     self.gate(f"open({str(self.marker)!r},'w').close()"))
        process = subprocess.Popen(self.command("--output", str(self.output), "--timeout", "3"),
                                   cwd=self.root, env=self.env, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, start_new_session=True)
        try:
            self.wait_for(lambda: ready.exists() and ready.stat().st_size > 0, process)
            self.track_pid(int(ready.read_text()))
            gate = int((self.output / "0001.stdout").read_text().strip())
            os.killpg(os.getpgid(gate), signal.SIGINT)
            out, err = process.communicate(timeout=8)
            self.assertEqual(process.returncode, 130, (out, err))
            self.assertFalse(self.marker.exists())
            self.assertFalse(Path(f"/proc/{int(ready.read_text())}").exists())
            self.assertEqual(self.events()[-1]["status"], "interrupted")
        finally:
            if process.poll() is None:
                process.terminate()
                process.communicate(timeout=5)

    def test_sigkill_leaves_recognizable_incomplete_start(self):
        # No active child is leaked by this test: the gate kills its runner,
        # writes a final byte, and exits itself.
        self.fixture(self.gate("import os,signal; os.kill(os.getppid(), signal.SIGKILL); os.write(1,b'end')"))
        result = self.execute()
        self.assertEqual(result.returncode, -signal.SIGKILL)
        self.assertEqual([e["event"] for e in self.events()], ["start"])
        self.assertTrue((self.output / "manifest.json").exists())

    def test_timeout_cleans_deadline_nested_sessions_not_unrelated_peer(self):
        ready = self.root / "nested-ready"
        nested = ("import os,signal,time; signal.signal(signal.SIGTERM,signal.SIG_IGN); "
                  f"open({str(ready)!r},'w').write(str(os.getpid())); time.sleep(60)")
        parent = ("import subprocess,time; "
                  f"subprocess.Popen({[sys.executable, '-c', nested]!r},start_new_session=True); "
                  "time.sleep(60)")
        self.fixture(registration("real deadline", sys.executable, str(ROOT / "tools/deadline.py"),
                                  "60", sys.executable, "-c", parent), self.gate("print('next')"))
        peer = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(60)"])
        process = subprocess.Popen(self.command("--output", str(self.output), "--timeout", "2"),
                                   cwd=self.root, env=self.env, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE)
        try:
            self.wait_for(lambda: ready.exists() and ready.stat().st_size > 0, process)
            self.track_pid(int(ready.read_text()))
            out, err = process.communicate(timeout=10)
            self.assertEqual(process.returncode, 1, (out, err))
            self.assertTrue(ready.exists())
            self.assertFalse(Path(f"/proc/{int(ready.read_text())}").exists())
            self.assertIsNone(peer.poll(), "runner must not kill or reap a sibling")
            results = [e for e in self.events() if e["event"] == "result"]
            self.assertEqual([e["status"] for e in results], ["timed_out", "exited_zero"])
            self.assertTrue(results[0]["timeout"])
            self.assertIsNone(results[0]["cleanup_error"])
        finally:
            peer.terminate()
            peer.wait(timeout=5)
            if process.poll() is None:
                process.terminate()
                process.communicate(timeout=5)

    def test_predictable_old_symlink_unfollowed(self):
        self.fixture(self.gate("pass"))
        victim = self.root / "victim"
        victim.write_bytes(b"unchanged")
        command = ["bash", str(MASTER), "--registry", str(self.registry), "--list"]
        # The exec keeps the PID used to preplant the historical snapshot name.
        shell = 'ln -s "$1" "$2/.test_suite.pinned.$$.sh"; shift 2; exec "$@"'
        env = dict(self.env)
        env.pop("JASH_SUITE_PINNED", None)
        result = subprocess.run(["bash", "-c", shell, "fixture", str(victim),
                                 str(self.root), *command], env=env,
                                cwd=self.root, capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(victim.read_bytes(), b"unchanged")

    def test_master_list_preserves_reviewed_registry_and_multiline_gate(self):
        result = subprocess.run(["bash", str(MASTER), "--list"], cwd=self.root,
                                env=self.env, capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        gates = [json.loads(line) for line in result.stdout.splitlines()]
        self.assertEqual(len(gates), 351)
        # Reviewed 2026-10-20: baseline 281 plus 70 registered structured gates;
        # digest unit path is the one intentional existing argv update.
        # Golden canonical argv digest after registering these gates,
        # exported by
        # Bash rather than parsing shell syntax. Pins order and duplicates too.
        self.assertEqual(hashlib.sha256(json.dumps(gates, sort_keys=True,
                         separators=(",", ":"), ensure_ascii=True).encode()).hexdigest(),
                            "e1cc98c242990826c161516014abca19ca0331503e3816fd506b15e31be6bf7f")
        self.assertEqual([g["index"] for g in gates], list(range(1, 352)))
        ir = next(g for g in gates if g["name"] == "agent runtime process supervisor and execution IR")
        self.assertEqual(ir["argv"], ["bash", "-c", "cmake --build build --target test_agent_process_ir >/dev/null && build/test_agent_process_ir"])
        self.assertEqual(sum(g["argv"] == ["bash", "tests/test_jinv096_function_word_names.sh"] for g in gates), 2)
        self.assertEqual(set(self.root.iterdir()), {self.root / "tests"})


if __name__ == "__main__":
    unittest.main()
