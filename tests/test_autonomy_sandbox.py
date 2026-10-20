#!/usr/bin/env python3
"""Standalone: python3 -B tests/test_autonomy_sandbox.py (77 = unavailable)."""

import json
import os
from pathlib import Path
import socket
import stat
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools.autonomy_sandbox import POLICY, build_argv


BWRAP = "/usr/bin/bwrap"


class ValidationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="autonomy-validation-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.work = self.root / "work space"
        self.work.mkdir()

    def test_malformed_commands(self):
        for command in (None, "true", (), [], [""], ["--help"], [1],
                        ["true", None], ["true\0"], ["true", "a\0b"]):
            with self.subTest(command=command), self.assertRaises(ValueError):
                build_argv(self.work, command, writable=True)

    def test_malformed_workspaces(self):
        regular = self.root / "file"
        regular.touch()
        link = self.root / "link"
        link.symlink_to(self.work, target_is_directory=True)
        for path in ("/tmp", Path("relative"), Path("/"), regular,
                     self.root / "missing", link, link / "..", Path("/tmp\0bad")):
            with self.subTest(path=path), self.assertRaises(ValueError):
                build_argv(path, ["true"], writable=True)

    def test_symlinked_workspace_ancestor_rejected(self):
        link = self.root / "alias"
        link.symlink_to(self.root, target_is_directory=True)
        with self.assertRaises(ValueError):
            build_argv(link / self.work.name, ["true"], writable=True)

    def test_writable_requires_bool(self):
        for writable in (None, 0, 1, "false"):
            with self.subTest(writable=writable), self.assertRaises(ValueError):
                build_argv(self.work, ["true"], writable=writable)

    def test_missing_backend_fails_closed(self):
        with self.assertRaises(OSError):
            build_argv(self.work, ["true"], writable=True,
                       bwrap=str(self.root / "missing"))

    def test_unavailable_backend_report_is_a_skip_not_a_pass(self):
        self.addCleanup(setattr, IsolationTests, "unavailable", IsolationTests.unavailable)
        suite = unittest.TestSuite([IsolationTests("test_command_arguments_are_literal")])
        result = unittest.TestResult()
        with mock.patch.object(subprocess, "run",
                               side_effect=FileNotFoundError("simulated missing bubblewrap")):
            suite.run(result)
        self.assertEqual(result.testsRun, 0)
        self.assertEqual(len(result.skipped), 1)
        self.assertIn("BACKEND UNAVAILABLE", result.skipped[0][1])
        self.assertIn("simulated missing bubblewrap", IsolationTests.unavailable)

    def test_malformed_backend_paths(self):
        for path in ("bwrap", "", "/usr/bin/bwrap\0", None):
            with self.subTest(path=path), self.assertRaises(ValueError):
                build_argv(self.work, ["true"], writable=True, bwrap=path)

    def test_untrusted_backend_rejected(self):
        launcher = self.root / "bwrap"
        launcher.write_text("#!/bin/sh\nexit 0\n")
        launcher.chmod(0o755)
        with self.assertRaises(PermissionError):
            build_argv(self.work, ["true"], writable=True, bwrap=str(launcher))

    def test_backend_must_be_regular(self):
        with self.assertRaises(PermissionError):
            build_argv(self.work, ["true"], writable=True, bwrap="/usr/bin")

    def test_backend_alias_returns_protected_canonical_executable(self):
        alias = self.root / "launcher-alias"
        alias.symlink_to("/usr/bin/true")
        argv = build_argv(self.work, ["true"], writable=True, bwrap=str(alias))
        self.assertEqual(argv[0], str(Path("/usr/bin/true").resolve()))

    def test_backend_metadata_and_ancestors_checked(self):
        # A root-owned executable is still replaceable under a writable parent.
        target = Path("/usr/bin/true").resolve(strict=True)
        real_stat = Path.stat
        for victim, uid, mode in ((target, 12345, 0o755),
                                  (target, 0, 0o775), (target, 0, 0o757),
                                  (target, 0, 0o644),
                                  (target.parent, 0, 0o777)):
            def metadata(path, *args, **kwargs):
                result = real_stat(path, *args, **kwargs)
                if path == victim:
                    values = list(result)
                    values[0] = stat.S_IFMT(result.st_mode) | mode
                    values[4] = uid
                    return os.stat_result(values)
                return result

            with self.subTest(victim=victim, uid=uid, mode=mode):
                with mock.patch.object(Path, "stat", metadata):
                    with self.assertRaises(PermissionError):
                        build_argv(self.work, ["true"], writable=True,
                                   bwrap=str(target))

    def test_literal_argv_and_fixed_policy(self):
        # No command is executed by the builder, including the launcher itself.
        command = ["printf", "%s", "a b; $(touch escaped)", "", "--bind"]
        with mock.patch.object(subprocess, "run", side_effect=AssertionError):
            argv = build_argv(self.work, command, writable=True,
                              bwrap="/usr/bin/true")
        separator = argv.index("--")
        self.assertEqual(argv[separator + 1:], command)
        self.assertEqual(command[0], "printf")
        for flag in ("--unshare-all", "--unshare-user", "--unshare-cgroup",
                     "--die-with-parent", "--new-session", "--as-pid-1", "--clearenv",
                     "--disable-userns", "--assert-userns-disabled"):
            self.assertIn(flag, argv[:separator])
        self.assertIn(["--bind", str(self.work), "/work"],
                      [argv[i:i + 3] for i in range(separator)])
        self.assertNotIn("--share-net", argv)
        self.assertNotIn("--dev-bind", argv)
        self.assertNotIn("/home", argv)
        self.assertNotIn("/etc", argv)
        self.assertNotIn("/", argv)
        self.assertIsInstance(POLICY, str)
        self.assertIn("kernel", POLICY)
        self.assertIn("supervisor", POLICY)


class IsolationTests(unittest.TestCase):
    unavailable = None

    @classmethod
    def setUpClass(cls):
        # Probe independently of build_argv: an implementation regression must
        # fail tests, not masquerade as an unavailable sandbox.
        reason = None
        env = {"PATH": "/usr/bin:/bin", "HOME": "/tmp", "LANG": "C"}
        try:
            help_result = subprocess.run([BWRAP, "--help"], env=env,
                                         capture_output=True, text=True, timeout=5)
            required = ("--disable-userns", "--assert-userns-disabled")
            if any(flag not in help_result.stdout for flag in required):
                reason = "bubblewrap lacks required nested-userns protection"
            else:
                probe = [BWRAP, "--unshare-all", "--unshare-user",
                         "--unshare-cgroup", *required, "--die-with-parent",
                         "--new-session", "--cap-drop", "ALL"]
                for name in ("/usr", "/bin", "/lib", "/lib64"):
                    if Path(name).exists():
                        probe += ["--ro-bind", str(Path(name).resolve()), name]
                result = subprocess.run(probe + ["--", "/usr/bin/true"],
                                        env=env, capture_output=True,
                                        text=True, timeout=5)
                if result.returncode:
                    reason = f"bubblewrap namespace probe rc={result.returncode}: {result.stderr.strip()}"
        except (OSError, subprocess.TimeoutExpired) as exc:
            reason = f"bubblewrap unavailable: {exc}"
        if reason:
            cls.unavailable = reason
            raise unittest.SkipTest("BACKEND UNAVAILABLE: " + reason)

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="autonomy-isolation-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.work = self.root / "work space"
        self.work.mkdir()
        self.sentinel = self.root / "outside"
        self.sentinel.write_text("UNCHANGED")
        self.home = self.root / "host-home"
        (self.home / ".ssh").mkdir(parents=True)
        (self.home / ".ssh" / "id_ed25519").write_text("PRIVATE-KEY-FIXTURE")
        self.env = dict(os.environ, HOME=str(self.home),
                        AUTONOMY_SECRET="DO-NOT-INHERIT", PYTHONPATH="/host/private")

    def run_python(self, code, *, writable=True, args=()):
        result = subprocess.run(
            build_argv(self.work, ["/usr/bin/python3", "-c", code, *args],
                       writable=writable), env=self.env, stdin=subprocess.DEVNULL,
            capture_output=True, text=True, timeout=10, close_fds=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout

    def test_writable_target_and_outside_sentinel(self):
        (self.work / "escape").symlink_to(self.sentinel)
        self.run_python("""
from pathlib import Path
import sys
assert Path.cwd() == Path('/work')
Path('generated').write_text('created')
for path in (Path(sys.argv[1]), Path('/work/escape'), Path('../outside')):
    try:
        path.write_text('attack')
    except OSError:
        pass
assert not Path('/home').exists()
assert not Path('/etc').exists()
""", args=[str(self.sentinel)])
        self.assertEqual((self.work / "generated").read_text(), "created")
        self.assertEqual(self.sentinel.read_text(), "UNCHANGED")

    def test_home_keys_environment_hidden(self):
        output = self.run_python("""
import json, os, sys
from pathlib import Path
assert not Path(sys.argv[1]).exists()
assert not Path('/tmp/.ssh/id_ed25519').exists()
assert not Path('/proc/1/root' + sys.argv[1]).exists()
assert b'DO-NOT-INHERIT' not in Path('/proc/self/environ').read_bytes()
assert b'DO-NOT-INHERIT' not in Path('/proc/1/environ').read_bytes()
print(json.dumps(dict(os.environ)))
""", args=[str(self.home)])
        # Python may coerce the C locale in its own environment.
        env = json.loads(output)
        self.assertEqual(env.pop("PATH"), "/usr/bin:/bin")
        self.assertEqual(env.pop("HOME"), "/tmp")
        self.assertEqual(env.pop("LANG"), "C")
        self.assertEqual(env.pop("PWD"), "/work")
        env.pop("LC_CTYPE", None)
        self.assertEqual(env, {})

    def test_command_arguments_are_literal(self):
        arguments = ["a b", "$(touch /work/escaped)", "; exit 1", "", "--bind"]
        output = self.run_python("import json, sys; print(json.dumps(sys.argv[1:]))",
                                 args=arguments)
        self.assertEqual(json.loads(output), arguments)
        self.assertFalse((self.work / "escaped").exists())

    def test_network_namespace_cannot_connect_host_listener(self):
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            listener.listen()
            port = listener.getsockname()[1]
            # Establish that the host service really is reachable outside.
            with socket.create_connection(("127.0.0.1", port), timeout=1):
                connection, _ = listener.accept()
                connection.close()
            self.run_python("""
import os, socket, sys
assert os.readlink('/proc/self/ns/net') != sys.argv[2]
# Socket creation is permitted; reaching the host network is not.
with socket.socket() as s:
    s.settimeout(1)
    assert s.connect_ex(('127.0.0.1', int(sys.argv[1]))) != 0
""", args=[str(port), os.readlink("/proc/self/ns/net")])

    def test_host_parent_pid_and_namespaces_inaccessible(self):
        names = ("user", "pid", "ipc", "uts", "cgroup", "mnt")
        host = {name: os.readlink(f"/proc/self/ns/{name}") for name in names}
        self.run_python("""
import errno, json, os, sys
from pathlib import Path
pid = int(sys.argv[1])
assert not Path('/proc/' + str(pid)).exists()
try:
    os.kill(pid, 0)
except ProcessLookupError:
    pass
else:
    raise AssertionError('host parent pid accessible')
for name, value in json.loads(sys.argv[2]).items():
    assert os.readlink('/proc/self/ns/' + name) != value, name
assert os.getsid(0) != int(sys.argv[3])
""", args=[str(os.getpid()), json.dumps(host), str(os.getsid(0))])

    def test_readonly_check_and_system_mounts(self):
        (self.work / "existing").write_text("original")
        self.run_python("""
import errno
from pathlib import Path
for path in ('/work/existing', '/work/new', '/usr/autonomy-write-probe',
             '/bin/autonomy-write-probe', '/lib/autonomy-write-probe'):
    try:
        Path(path).write_text('attack')
    except OSError as exc:
        assert exc.errno in (errno.EROFS, errno.EACCES), (path, exc)
    else:
        raise AssertionError('write permitted: ' + path)
Path('/tmp/check-output').write_text('private scratch is writable')
""", writable=False)
        self.assertEqual((self.work / "existing").read_text(), "original")
        self.assertFalse((self.work / "new").exists())
        self.assertFalse((self.root / "check-output").exists())

    def test_mount_escape_and_nested_userns_denied(self):
        self.run_python("""
import ctypes, os
from pathlib import Path
libc = ctypes.CDLL(None, use_errno=True)
status = dict(line.split(':', 1) for line in Path('/proc/self/status').read_text().splitlines())
for name in ('CapInh', 'CapPrm', 'CapEff', 'CapBnd', 'CapAmb'):
    assert int(status[name], 16) == 0, name
assert int(status['NoNewPrivs']) == 1
assert libc.unshare(0x10000000) == -1  # CLONE_NEWUSER
assert libc.unshare(0x00020000) == -1  # CLONE_NEWNS
assert libc.mount(None, b'/work', None, 32, None) == -1  # MS_REMOUNT
fd = os.open('/proc/1/ns/mnt', os.O_RDONLY)
assert libc.setns(fd, 0) == -1
os.close(fd)
""", writable=False)

    def test_detached_children_die_on_command_exit_and_launcher_kill(self):
        code = """
import os, sys, time
from pathlib import Path
r, w = os.pipe()
if os.fork() == 0:
    os.close(r)
    os.setsid()
    if os.fork() != 0:
        os._exit(0)
    Path('/work/ready').touch()
    os.write(w, b'1')
    os.close(w)
    while True:
        Path('/work/heartbeat').write_text(str(time.monotonic_ns()))
        time.sleep(0.02)
os.close(w)
assert os.read(r, 1) == b'1'
if sys.argv[1] == 'wait':
    time.sleep(30)
"""
        for mode in ("exit", "wait", "parent-exit"):
            with self.subTest(mode=mode):
                (self.work / "ready").unlink(missing_ok=True)
                argv = build_argv(self.work, ["/usr/bin/python3", "-c", code,
                                              "exit" if mode == "exit" else "wait"],
                                  writable=True)
                if mode == "parent-exit":
                    # This host helper is only a test of --die-with-parent. Its
                    # inherited output pipes must reach EOF despite double-fork.
                    argv = [sys.executable, "-c", """
import json, subprocess, sys
child = subprocess.Popen(json.loads(sys.argv[1]), stdin=subprocess.DEVNULL)
sys.stdin.buffer.read(1)
""", json.dumps(argv)]
                proc = subprocess.Popen(argv, env=self.env,
                                        stdin=subprocess.PIPE if mode == "parent-exit" else subprocess.DEVNULL,
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                        text=True, close_fds=True)
                try:
                    if mode != "exit":
                        deadline = time.monotonic() + 5
                        while not (self.work / "ready").exists():
                            self.assertIsNone(proc.poll(), "sandbox exited before ready")
                            self.assertLess(time.monotonic(), deadline)
                            time.sleep(0.01)
                        if mode == "wait":
                            proc.kill()
                    out, err = proc.communicate(input="x" if mode == "parent-exit" else None,
                                                timeout=5)
                    self.assertEqual(proc.returncode, -9 if mode == "wait" else 0, out + err)
                    self.assertTrue((self.work / "ready").exists())
                    heartbeat = self.work / "heartbeat"
                    before = heartbeat.read_bytes() if heartbeat.exists() else None
                    time.sleep(0.15)
                    self.assertEqual(heartbeat.read_bytes() if heartbeat.exists() else None,
                                     before, "detached child survived namespace teardown")
                finally:
                    if proc.poll() is None:
                        proc.kill()
                    proc.communicate(timeout=5)


if __name__ == "__main__":
    suite = unittest.main(verbosity=2, exit=False)
    if not suite.result.wasSuccessful():
        sys.exit(1)
    if IsolationTests.unavailable:
        print("BACKEND UNAVAILABLE: isolation NOT verified", file=sys.stderr)
        sys.exit(77)
