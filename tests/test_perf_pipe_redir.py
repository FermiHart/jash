#!/usr/bin/env python3
"""Waves 7-8 regressions. JASH selects an existing binary; no shell build/benchmark.

The isolated C unit sets CLOEXEC after exec: subprocess pass_fds deliberately
clears it, so passing a CLOEXEC fd to a shell cannot test flag restoration.
"""

import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parent.parent
SHELL = Path(os.environ.get("JASH", ROOT / "build/jash")).resolve()

REDIR_UNIT = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <sys/syscalls.h>

static int setfd_calls, fail_command = -1, malloc_calls, fail_malloc;
static int fail_realloc;
static void* tracked_realloc(void* p, size_t n) {
    if (fail_realloc) { errno = ENOMEM; return NULL; }
    return realloc(p, n);
}
static void* tracked_malloc(size_t n) {
    malloc_calls++;
    if (fail_malloc && malloc_calls == fail_malloc) {
        errno = ENOMEM;
        return NULL;
    }
    return malloc(n);
}
static int tracked_fcntl(int fd, int cmd, ...) {
    int arg = 0;
    if (cmd != F_GETFD) {
        va_list ap;
        va_start(ap, cmd);
        arg = va_arg(ap, int);
        va_end(ap);
    }
    if (cmd == F_SETFD) setfd_calls++;
    if (cmd == fail_command) { errno = EIO; return -1; }
    return fcntl(fd, cmd, arg);
}
#define malloc tracked_malloc
#define realloc tracked_realloc
#define fcntl tracked_fcntl
#include "parser/builtin_redir.c"
#undef malloc
#undef realloc
#undef fcntl

static const char* expected_args;
static int dispatched;
int jash_builtin_dispatch(const jash_builtin_t* b, jash_state_t* s,
                          jash_writer_t w, void* ctx, const char* args) {
    (void)b; (void)s; (void)w; (void)ctx;
    assert(strcmp(args, expected_args) == 0);
    dispatched++;
    return 0;
}
void jash_timeline_journal_redirect(const char* path, int append) {
    (void)path; (void)append;
}
jmod_table_t g_modules;
int jash_b_read(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    (void)s; (void)w; (void)ctx; (void)args;
    assert(0);
    return 1;
}
int jash_b_read_from_fd(jash_state_t* s, jash_writer_t w, void* ctx, const char* args, int fd) {
    (void)fd;
    return jash_b_read(s, w, ctx, args);
}
int jash_b_printf(jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv) {
    (void)s; (void)w; (void)ctx; (void)argc; (void)argv;
    assert(0); /* This unit uses synthetic, non-native builtins. */
    return 1;
}
int jash_b_echo_argv(jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv) {
    return jash_b_printf(s, w, ctx, argc, argv);
}
static int dummy_argv(jash_state_t* s, jash_writer_t w, void* ctx,
                       int argc, char** argv) {
    (void)s; (void)w; (void)ctx; (void)argc; (void)argv;
    return 0;
}
int main(int argc, char** argv) {
    assert(argc >= 2);
    if (strcmp(argv[1], "buffer-growth") == 0) {
        FILE* output = tmpfile();
        assert(output);
        char small[8];
        br_obuf_t buffer = {.buf = small, .cap = sizeof small, .fd = fileno(output)};
        assert(br_obuf_write(&buffer, "prefix", 6) == 6);
        fail_realloc = atoi(argv[2]);
        assert(br_obuf_write(&buffer, ":suffix", 7) == 7);
        if (buffer.len) assert(br_write_all(buffer.fd, buffer.buf, buffer.len) == 0);
        assert(buffer.degraded == fail_realloc && !buffer.error);
        rewind(output);
        char actual[16] = {0};
        assert(fread(actual, 1, sizeof actual, output) == 13);
        assert(strcmp(actual, "prefix:suffix") == 0);
        free(buffer.heap);
        fclose(output);
        return 0;
    }
    if (strcmp(argv[1], "snapshot-depth") == 0) {
        int target = open("/dev/null", O_RDWR);
        assert(target >= 0);
        for (int round = 0; round < 3; round++) {
            jash_redir_snapshot_t* frames[12];
            for (int i = 0; i < 12; i++) {
                assert(jash_redir_snapshot_begin(&frames[i], &target, 1, NULL, 0) == 0);
            }
            for (int i = 11; i >= 0; i--) assert(jash_redir_snapshot_end(frames[i], 1) == 0);
            assert(!br_active);
        }
        int targets[20];
        for (int i = 0; i < 20; i++) targets[i] = target;
        jash_redir_snapshot_t* wide;
        assert(jash_redir_snapshot_begin(&wide, targets, 20, NULL, 0) == 0);
        assert(wide->count == 1);
        assert(jash_redir_snapshot_end(wide, 1) == 0);
        assert(!br_active);
        close(target);
        return 0;
    }
    if (strcmp(argv[1], "scratch") == 0) {
        assert(argc == 5);
        int quoted = atoi(argv[3]), fail = atoi(argv[4]);
        jash_builtin_t b = {.name = argv[2]};
        if (strcmp(b.name, "argv") == 0) b.argv_fn = dummy_argv;
        static jash_state_t state;
        char line[8192], raw[4096], unquoted[4096];
        memset(unquoted, 'x', 3000);
        unquoted[3000] = 0;
        snprintf(raw, sizeof raw, quoted ? "'%s'" : "%s", unquoted);
        snprintf(line, sizeof line, "%s %s > /dev/null", b.name, raw);
        int needs_unquote = quoted && strcmp(b.name, "legacy") == 0;
        expected_args = needs_unquote ? unquoted : raw;
        fail_malloc = fail ? 2 : 0; /* clean succeeds; only scratch may fail */
        int rc = jash_eval_builtin_with_redir(&state, NULL, NULL, &b, line);
        assert(rc == (fail && needs_unquote));
        assert(dispatched == !(fail && needs_unquote));
        assert(malloc_calls == 1 + needs_unquote);
        assert(br_active == NULL);
        return 0;
    }
    int fd = open("/dev/null", O_RDWR);
    assert(fd >= 0);
    int flags = atoi(argv[2]);
    assert(fcntl(fd, F_SETFD, flags) == 0);
    jash_redir_snapshot_t* saved = NULL;
    if (strcmp(argv[1], "save-failure") == 0) {
        fail_command = flags ? F_DUPFD_CLOEXEC : F_GETFD;
        assert(jash_redir_snapshot_begin(&saved, &fd, 1, NULL, 0) == 1);
        assert(saved == NULL && br_active == NULL);
        assert(fcntl(fd, F_GETFD) == flags);
    } else {
        int closed = strcmp(argv[1], "closed") == 0;
        if (closed) assert(close(fd) == 0);
        assert(jash_redir_snapshot_begin(&saved, &fd, 1, NULL, 0) == 0);
        if (closed) assert(open("/dev/null", O_RDWR) == fd);
        else assert(close(fd) == 0);
        if (strcmp(argv[1], "nested") == 0) {
            int backup = saved->entries[0].fd;
            jash_redir_snapshot_t* inner;
            assert(jash_redir_snapshot_begin(&inner, &backup, 1, NULL, 0) == 0);
            assert(saved->entries[0].fd != backup);
            assert(dup2(saved->entries[0].fd, backup) == backup);
            assert(jash_redir_snapshot_end(inner, 1) == 0);
            assert(fcntl(backup, F_GETFD) == -1 && errno == EBADF);
        }
        int fail = strcmp(argv[1], "restore-failure") == 0;
        if (fail) fail_command = F_SETFD;
        setfd_calls = 0;
        assert(jash_redir_snapshot_end(saved, 1) == (fail && flags != 0));
        assert(setfd_calls == (!closed && flags != 0));
        assert(br_active == NULL);
        if (closed) assert(fcntl(fd, F_GETFD) == -1 && errno == EBADF);
        else assert(fcntl(fd, F_GETFD) == (fail ? 0 : flags));
    }
    close(fd);
    return 0;
}
'''


PIPE_UNIT = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <sys/syscalls.h>

static int pipe2_error, pipe_error, fail_setfd, pipe2_calls, pipe_calls, setfd_calls;
static int opened[2] = {-1, -1};
static int tracked_pipe2(int fds[2], int flags) {
    pipe2_calls++;
    assert(flags == O_CLOEXEC);
    if (pipe2_error) { errno = pipe2_error; return -1; }
    return pipe2(fds, flags);
}
static long tracked_pipe(int fds[2]) {
    pipe_calls++;
    if (pipe_error) { errno = pipe_error; return -1; }
    int rc = pipe(fds);
    if (!rc) { opened[0] = fds[0]; opened[1] = fds[1]; }
    return rc;
}
static int tracked_fcntl(int fd, int cmd, ...) {
    assert(cmd == F_SETFD);
    va_list ap;
    va_start(ap, cmd);
    int arg = va_arg(ap, int);
    va_end(ap);
    assert(arg == FD_CLOEXEC);
    if (++setfd_calls == fail_setfd) { errno = EIO; return -1; }
    return fcntl(fd, cmd, arg);
}
#define pipe2 tracked_pipe2
#define sys_pipe tracked_pipe
#define fcntl tracked_fcntl
#ifdef PORTABLE_PIPE
#undef __linux__
#endif
#include "parser/pipeline.c"
#undef pipe2
#undef sys_pipe
#undef fcntl

int main(int argc, char** argv) {
    assert(argc == 4);
    pipe2_error = atoi(argv[1]);
    pipe_error = atoi(argv[2]);
    fail_setfd = atoi(argv[3]);
    int fds[2] = {-1, -1};
    int rc = pipe_cloexec(fds);
    int error = errno;
#ifdef PORTABLE_PIPE
    int fallback = 1;
    assert(pipe2_calls == 0);
#else
    int fallback = pipe2_error == ENOSYS;
    assert(pipe2_calls == 1);
#endif
    assert(pipe_calls == fallback);
    assert(setfd_calls == (fallback && !pipe_error ? (fail_setfd == 1 ? 1 : 2) : 0));
    int expected_error = fallback ? (pipe_error ? pipe_error : (fail_setfd ? EIO : 0))
                                  : pipe2_error;
    assert(rc == (expected_error ? -1 : 0));
    if (rc == 0) {
        assert(fcntl(fds[0], F_GETFD) == FD_CLOEXEC);
        assert(fcntl(fds[1], F_GETFD) == FD_CLOEXEC);
        close(fds[0]);
        close(fds[1]);
    } else {
        assert(error == expected_error);
        for (int i = 0; i < 2; i++)
            if (opened[i] >= 0)
                assert(fcntl(opened[i], F_GETFD) == -1 && errno == EBADF);
    }
    return 0;
}
'''


@unittest.skipUnless(sys.platform.startswith("linux"), "isolated GNU linker unit")
class PipeRedirUnit(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = shlex.split(os.environ.get("CC", "cc"))
        if not cc or not shutil.which(cc[0]):
            raise unittest.SkipTest("C compiler unavailable")
        tmp = tempfile.TemporaryDirectory(prefix="jash-perf-redir-unit-")
        cls.addClassCleanup(tmp.cleanup)
        cls.binary = Path(tmp.name) / "redir-unit"
        cls.pipe_binary = Path(tmp.name) / "pipe-unit"
        cls.portable_binary = Path(tmp.name) / "pipe-portable-unit"
        for binary, source, extras in (
            (cls.binary, REDIR_UNIT, [str(ROOT / "c/parser/quotes.c"),
                                     str(ROOT / "c/parser/cmdsub.c")]),
            (cls.pipe_binary, PIPE_UNIT, []),
            (cls.portable_binary, PIPE_UNIT, ["-DPORTABLE_PIPE"]),
        ):
            proc = subprocess.run(
                [*cc, "-std=gnu11", "-O0", "-ffunction-sections", "-fdata-sections",
                 "-I", str(ROOT / "c"), "-I", str(ROOT / "c/sys"),
                 "-x", "c", "-", *extras, "-Wl,--gc-sections", "-o", str(binary)],
                input=source, text=True, capture_output=True, timeout=30)
            if proc.returncode:
                raise AssertionError(proc.stderr)

    def unit(self, *args, binary=None):
        proc = subprocess.run([str(binary or self.binary), *map(str, args)],
                              capture_output=True, timeout=10)
        self.assertEqual(proc.returncode, 0, proc.stderr.decode())

    def test_flags_closed_targets_relocation_and_fcntl_failures(self):
        for mode in ("restore", "nested", "closed", "save-failure", "restore-failure"):
            for flags in (0, 1):  # Linux FD_CLOEXEC, set inside the unit after exec
                with self.subTest(mode=mode, flags=flags):
                    self.unit(mode, flags)

    def test_scratch_only_allocated_for_legacy_quoted_args(self):
        for name in ("printf", "argv", "legacy"):
            for quoted in (0, 1):
                for fail in (0, 1):
                    with self.subTest(name=name, quoted=quoted, fail=fail):
                        self.unit("scratch", name, quoted, fail)

    def test_snapshots_preserve_deep_and_wide_scopes(self):
        self.unit("snapshot-depth")

    def test_output_inline_growth_and_allocation_failure(self):
        self.unit("buffer-growth", 0)
        self.unit("buffer-growth", 1)

    def test_pipe2_and_checked_portable_fallback(self):
        import errno

        for binary in (self.pipe_binary, self.portable_binary):
            for pipe2_error in (0, errno.ENOSYS, errno.EPERM, errno.EINVAL):
                for pipe_error, fail_setfd in ((0, 0), (errno.EMFILE, 0), (0, 1), (0, 2)):
                    with self.subTest(binary=binary.name, pipe2_error=pipe2_error,
                                      pipe_error=pipe_error, fail_setfd=fail_setfd):
                        self.unit(pipe2_error, pipe_error, fail_setfd, binary=binary)


class PipeRedirBehavior(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory(prefix="jash-perf-pipe-redir-")
        self.addCleanup(tmp.cleanup)
        self.work = Path(tmp.name)
        self.env = {
            "HOME": str(self.work), "TMPDIR": str(self.work),
            "PATH": "/usr/bin:/bin", "LC_ALL": "C", "TERM": "dumb",
            "JASH_SIGIL": "0", "JASH_CHANGES": "0", "CHANGES": "0",
            "JASH_SAFE": "0", "JASH_BRAIN": "0", "JASH_WORLD": "0",
            "JASH_MEMO": "0", "JASH_BRIEF": "0", "JASH_NUDGE": "0",
            "JASH_LOOPGUARD": "off", "JASH_DRIFT": "0",
            "JASH_RECALL_GUARD": "0", "JASH_RISK_CHECKPOINT": "0",
            "JASH_GUARD": "0", "JASH_GUARD_STRICT": "0",
            "JASH_GUARD_BYPASS": "0", "JASH_GUARD_BYPASS_STRICT": "0",
            "JASH_SKIP_SELFCHECK": "1", "JASH_ABSORB": "0",
            "ASAN_OPTIONS": "detect_leaks=0:abort_on_error=1",
            "UBSAN_OPTIONS": "halt_on_error=1",
        }

    def shell(self, code, *, stdin=b"", pass_fds=()):
        if SHELL.name == "bash":
            code = "shopt -s lastpipe; " + code
        proc = subprocess.run([str(SHELL), "-c", code], input=stdin,
                              capture_output=True, cwd=self.work, env=self.env,
                              pass_fds=pass_fds, timeout=15)
        return proc.returncode, proc.stdout, proc.stderr

    def test_all_builtin_then_external_and_assignment_environments(self):
        self.assertEqual(self.shell(
            "export WAVE_ENV=parent; printf 'hello\\n' | read value; "
            "printf '<%s>' \"$value\"; "
            "env | /bin/grep '^WAVE_ENV='; "
            "WAVE_ENV=child env | /bin/grep '^WAVE_ENV='; "
            "WAVE_ENV=fork /usr/bin/printenv WAVE_ENV | cat; "
            "true | WAVE_ENV=last /usr/bin/printenv WAVE_ENV; "
            "true | /usr/bin/printenv WAVE_ENV; "
            "unset WAVE_ENV; true | /usr/bin/env | /bin/grep '^WAVE_ENV='"),
            (1, b"<hello>WAVE_ENV=parent\nWAVE_ENV=child\nfork\nlast\nparent\n", b""))

    def test_builtin_wrapper_can_launch_external_with_current_env(self):
        for wrapper in ("command", "env", "builtin command"):
            with self.subTest(wrapper=wrapper):
                self.assertEqual(self.shell(
                    f"export WAVE_ENV=wrapped; true | {wrapper} "
                    "/usr/bin/printenv WAVE_ENV"), (0, b"wrapped\n", b""))

    @unittest.skipIf(SHELL.name == "bash", "Jash absorption policy")
    def test_absorbed_environment_and_assignment_rebuild(self):
        self.env.update(JASH_ABSORB="1", JASH_ADOPT="3", JASH_SIGIL="1",
                        JASH_SIGIL_ALLOW_DEGRADED="1")
        rc, out, err = self.shell(
            "export WAVE_ENV=parent; true | bash -c "
            "'/usr/bin/printenv WAVE_ENV JASH_ABSORBED'; "
            "WAVE_ENV=child bash -c '/usr/bin/printenv WAVE_ENV JASH_ABSORBED' | cat")
        self.assertEqual((rc, out), (0, b"parent\n1\nchild\n1\n"), err)
        self.assertIn(b"pipeline segment absorbed", err)

    def test_long_printf_and_argv_redirects(self):
        payload = "a b \"quoted\" " * 1000
        for cmd in ("printf '%s'", "echo -n"):
            with self.subTest(cmd=cmd):
                self.assertEqual(self.shell(
                    f"{cmd} {shlex.quote(payload)} > output; printf AFTER"),
                    (0, b"AFTER", b""))
                self.assertEqual((self.work / "output").read_bytes(), payload.encode())

    def test_large_nested_wrapper_output_order_and_stdin_sync(self):
        self.env["JASH_LLM_BUDGET"] = "128"
        payload = b"line with spaces\n" * 10000
        (self.work / "input").write_bytes(payload)
        self.assertEqual(self.shell(
            'builtin eval "printf BEFORE; command cat <input; '
            'builtin eval \'printf INNER >&2\'; printf AFTER" >output 2>errors; '
            'cat <input >copy; read value; printf "%s" "$value"',
            stdin=b"ORIGINAL\n"), (0, b"ORIGINAL", b""))
        self.assertEqual((self.work / "output").read_bytes(), b"BEFORE" + payload + b"AFTER")
        self.assertEqual((self.work / "errors").read_bytes(), b"INNER")
        self.assertEqual((self.work / "copy").read_bytes(), payload)

    def test_large_builtin_and_external_pipelines(self):
        payload = b"pipeline-data\n" * 10000
        (self.work / "input").write_bytes(payload)
        for budget in ("0", "128"):
            self.env["JASH_LLM_BUDGET"] = budget
            for command in ("cat", "/bin/cat"):
                with self.subTest(command=command, budget=budget):
                    self.assertEqual(self.shell(
                        f"{command} input | {command} | {command} >output; printf AFTER"),
                        (0, b"AFTER", b""))
                    self.assertEqual((self.work / "output").read_bytes(), payload)

    def test_passed_inheritable_fd_survives_nested_restore(self):
        with (self.work / "passed").open("wb") as target:
            fd = target.fileno()
            self.assertEqual(self.shell(
                f'printf X | builtin eval "exec 10>/dev/null" {fd}>temporary; '
                f'/bin/echo RESTORED >&{fd}', pass_fds=(fd,)), (0, b"", b""))
        self.assertEqual((self.work / "passed").read_bytes(), b"RESTORED\n")

    def test_write_and_setup_failures_still_restore(self):
        redirects = [">missing/output", "9>&- 1>&9"]
        if Path("/dev/full").exists():
            redirects.append(">/dev/full")
        for redir in redirects:
            with self.subTest(redir=redir):
                rc, out, err = self.shell(f"printf BAD {redir}; rc=$?; printf AFTER; exit $rc")
                self.assertNotEqual(rc, 0)
                self.assertEqual(out, b"AFTER")
                self.assertTrue(err)


if __name__ == "__main__":
    unittest.main()
