#!/usr/bin/env bash
# Security regressions for interactive heredocs, session.key, and proc snapshots.
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
J="${JASH_UNDER_TEST:-${JASH:-$ROOT/build/jash}}"
[ -x "$J" ] || { echo "FAIL: jash binary missing or not executable: $J"; exit 1; }
for source in "$ROOT/c/main.c" "$ROOT/c/module/jash_crypto.c" "$ROOT/c/core/timeline.c"; do
    [ "$source" -nt "$J" ] || continue
    echo "FAIL: stale jash binary: $J is older than $source"
    exit 1
done
command -v cc >/dev/null 2>&1 || { echo "FAIL: cc missing"; exit 1; }
command -v python3 >/dev/null 2>&1 || { echo "FAIL: python3 missing"; exit 1; }

T="$(mktemp -d)" || exit 2
PIDS=""
cleanup() {
    [ -n "$PIDS" ] && kill $PIDS 2>/dev/null || true
    rm -rf "$T"
}
trap cleanup EXIT INT TERM

PASS=0
FAIL=0
pass() { PASS=$((PASS + 1)); printf 'ok    %s\n' "$1"; }
fail() { FAIL=$((FAIL + 1)); printf 'FAIL  %s\n        %s\n' "$1" "$2"; }

cat >"$T/key_probe.c" <<'EOF_C'
#include <module/jash_crypto.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

int __real_openat(int, const char *, int, ...);
ssize_t __real_write(int, const void *, size_t);

int __wrap_openat(int dirfd, const char *path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }
    int fd = (flags & O_CREAT) ? __real_openat(dirfd, path, flags, mode)
                               : __real_openat(dirfd, path, flags);
    int saved = errno;
    const char *barrier = getenv("KEY_BARRIER");
    if (fd >= 0 && barrier && strcmp(path, ".session.key.lock") == 0) {
        char checked[512], go[512];
        snprintf(checked, sizeof checked, "%s/checked.%ld", barrier, (long)getpid());
        FILE *marker = fopen(checked, "w");
        if (marker) fclose(marker);
        snprintf(go, sizeof go, "%s/go", barrier);
        for (int i = 0; i < 5000 && access(go, F_OK) != 0; i++) usleep(1000);
    }
    errno = saved;
    return fd;
}

ssize_t __wrap_write(int fd, const void *buf, size_t count) {
    static int fault_stage = 0;
    if (getenv("KEY_FAIL_WRITE") && count <= 64 && count > 1) {
        if (fault_stage++ == 0) return __real_write(fd, buf, 7);
        errno = EIO;
        return -1;
    }
    return __real_write(fd, buf, count);
}

int main(void) {
    alarm(10);
    const char *barrier = getenv("KEY_BARRIER");
    if (barrier) {
        char ready[512], start[512];
        snprintf(ready, sizeof ready, "%s/ready.%ld", barrier, (long)getpid());
        FILE *f = fopen(ready, "w");
        if (f) fclose(f);
        snprintf(start, sizeof start, "%s/start", barrier);
        int i;
        for (i = 0; i < 5000 && access(start, F_OK) != 0; i++) usleep(1000);
        if (i == 5000) return 3;
    }
    uint8_t key[JASH_SHA256_DIGEST_LEN];
    if (jash_session_key_load(key) != 0) return 1;
    for (size_t i = 0; i < sizeof key; i++) printf("%02x", key[i]);
    putchar('\n');
    return 0;
}
EOF_C

cc -I"$ROOT/c" -Wl,--wrap=openat -Wl,--wrap=write -o "$T/key_probe" \
    "$T/key_probe.c" "$ROOT/c/module/jash_crypto.c" "$ROOT/c/module/blake3.c" || exit 2

# All first-use contenders must return the key that remains on disk.
KEY_HOME="$T/key-home"
BARRIER="$T/key-barrier"
mkdir -m 700 "$KEY_HOME" "$KEY_HOME/.jash" "$BARRIER"
KEY_PIDS=""
barrier_ready=0
barrier_checked=0
for i in $(seq 1 16); do
    HOME="$KEY_HOME" KEY_BARRIER="$BARRIER" "$T/key_probe" >"$T/key.$i" 2>&1 &
    KEY_PIDS="$KEY_PIDS $!"
done
for unused in $(seq 1 200); do
    set -- "$BARRIER"/ready.*
    if [ -e "$1" ] && [ "$#" -eq 16 ]; then barrier_ready=1; break; fi
    sleep 0.01
done
: >"$BARRIER/start"
for unused in $(seq 1 100); do
    set -- "$BARRIER"/checked.*
    if [ -e "$1" ] && [ "$#" -eq 16 ]; then barrier_checked=1; break; fi
    sleep 0.01
done
: >"$BARRIER/go"
key_wait_rc=0
[ "$barrier_ready" -eq 1 ] && [ "$barrier_checked" -eq 1 ] || key_wait_rc=1
for pid in $KEY_PIDS; do wait "$pid" || key_wait_rc=1; done
disk_key="$(cat "$KEY_HOME/.jash/session.key" 2>/dev/null || true)"
all_keys="$(cat "$T"/key.* 2>/dev/null || true)"
key_lines="$(printf '%s\n' "$all_keys" | grep -c '^[0-9a-f]\{64\}$' || true)"
key_unique="$(printf '%s\n' "$all_keys" | sort -u | wc -l | tr -d ' ')"
only_key="$(printf '%s\n' "$all_keys" | sort -u)"
if [ "$key_wait_rc" -eq 0 ] && [ "$key_lines" -eq 16 ] && \
   [ "$key_unique" -eq 1 ] && [ "$only_key" = "$disk_key" ]; then
    pass "concurrent session.key first use converges on the on-disk key"
else
    fail "concurrent session.key first use converges" \
         "wait=$key_wait_rc valid=$key_lines unique=$key_unique disk=$disk_key outputs=[$all_keys]"
fi

# A failed short write must never publish a malformed final key.
FAULT_KEY_HOME="$T/key-fault-home"
mkdir -m 700 "$FAULT_KEY_HOME" "$FAULT_KEY_HOME/.jash"
if HOME="$FAULT_KEY_HOME" KEY_FAIL_WRITE=1 "$T/key_probe" >/dev/null 2>&1; then
    fail "session.key short write is failure-atomic" "key loader returned success"
elif [ -e "$FAULT_KEY_HOME/.jash/session.key" ]; then
    fail "session.key short write is failure-atomic" \
         "malformed final key was committed ($(wc -c <"$FAULT_KEY_HOME/.jash/session.key") bytes)"
else
    pass "session.key short write leaves no committed key"
fi

# A malformed private key is an explicit failure, never an unlink-and-replace.
printf 'not-a-complete-key' >"$KEY_HOME/.jash/session.key"
chmod 600 "$KEY_HOME/.jash/session.key"
before="$(cat "$KEY_HOME/.jash/session.key")"
if HOME="$KEY_HOME" "$T/key_probe" >/dev/null 2>&1; then
    fail "malformed session.key is not replaced" "key loader returned success"
elif [ "$(cat "$KEY_HOME/.jash/session.key")" != "$before" ]; then
    fail "malformed session.key is not replaced" "key contents changed"
else
    pass "malformed session.key fails without unlinking it"
fi

printf '%064d' 0 >"$KEY_HOME/.jash/session.key"
chmod 0644 "$KEY_HOME/.jash/session.key"
if HOME="$KEY_HOME" "$T/key_probe" >/dev/null 2>&1; then
    fail "group/world-accessible session.key is rejected" "key loader returned success"
else
    pass "group/world-accessible session.key is rejected"
fi

printf 'DO-NOT-TOUCH' >"$T/key-victim"
rm -f "$KEY_HOME/.jash/session.key"
ln -s "$T/key-victim" "$KEY_HOME/.jash/session.key"
if HOME="$KEY_HOME" "$T/key_probe" >/dev/null 2>&1 || \
   [ "$(cat "$T/key-victim")" != DO-NOT-TOUCH ]; then
    fail "symlinked session.key is rejected" "loader changed or accepted the symlink target"
else
    pass "symlinked session.key is rejected without touching its target"
fi

# The trusted directory itself must not be a symlink or expose the key.
LINK_HOME="$T/link-home"
LINK_TARGET="$T/link-target"
mkdir -m 700 "$LINK_HOME" "$LINK_TARGET"
ln -s "$LINK_TARGET" "$LINK_HOME/.jash"
if HOME="$LINK_HOME" "$T/key_probe" >/dev/null 2>&1 || [ -e "$LINK_TARGET/session.key" ]; then
    fail "symlinked .jash directory is rejected" "loader followed .jash and created a key"
else
    pass "symlinked .jash directory is rejected"
fi

cat >"$T/io_fault.c" <<'EOF_C'
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fd_matches(int fd, const char *exact, const char *part) {
    char proc[64], path[PATH_MAX];
    snprintf(proc, sizeof proc, "/proc/self/fd/%d", fd);
    ssize_t n = readlink(proc, path, sizeof path - 1);
    if (n < 0) return 0;
    path[n] = '\0';
    return (exact && strcmp(path, exact) == 0) || (part && strstr(path, part));
}

int dup(int fd) {
    static int (*real_dup)(int) = 0;
    if (!real_dup) real_dup = dlsym(RTLD_NEXT, "dup");
    if (getenv("HEREDOC_FAIL_DUP") && fd_matches(fd, 0, "jash_heredoc_")) {
        errno = EMFILE;
        return -1;
    }
    return real_dup(fd);
}

ssize_t write(int fd, const void *buf, size_t count) {
    static ssize_t (*real_write)(int, const void *, size_t) = 0;
    static int heredoc_stage = 0, snapshot_stage = 0;
    if (!real_write) real_write = dlsym(RTLD_NEXT, "write");
    if (getenv("HEREDOC_FAIL_WRITE") && fd_matches(fd, 0, "jash_heredoc_")) {
        if (heredoc_stage++ == 0 && count > 1)
            return real_write(fd, buf, count / 2);
        errno = EIO;
        return -1;
    }
    if (getenv("SNAP_FAIL_WRITE") &&
        fd_matches(fd, getenv("SNAP_TARGET"), ".jash-snapshot.")) {
        if (snapshot_stage++ == 0 && count > 1)
            return real_write(fd, buf, count / 2);
        errno = EIO;
        return -1;
    }
    return real_write(fd, buf, count);
}

int close(int fd) {
    static int (*real_close)(int) = 0;
    if (!real_close) real_close = dlsym(RTLD_NEXT, "close");
    int fail = getenv("HEREDOC_FAIL_CLOSE") &&
               fd_matches(fd, 0, "jash_heredoc_");
    int rc = real_close(fd);
    if (fail) { errno = EIO; return -1; }
    return rc;
}

int fsync(int fd) {
    static int (*real_fsync)(int) = 0;
    static int failed = 0;
    static int snapshot_dir_syncs = 0;
    if (!real_fsync) real_fsync = dlsym(RTLD_NEXT, "fsync");
    if (getenv("SNAP_FAIL_CLEANUP_FSYNC") &&
        fd_matches(fd, getenv("SNAP_DIR"), 0) &&
        ++snapshot_dir_syncs == 2) {
        errno = EIO;
        return -1;
    }
    if (!failed && getenv("SNAP_FAIL_DIR_FSYNC") &&
        fd_matches(fd, getenv("SNAP_DIR"), 0)) {
        failed = 1;
        errno = EIO;
        return -1;
    }
    return real_fsync(fd);
}

int unlinkat(int dirfd, const char *path, int flags) {
    static int (*real_unlinkat)(int, const char *, int) = 0;
    if (!real_unlinkat) real_unlinkat = dlsym(RTLD_NEXT, "unlinkat");
    if (getenv("HEREDOC_FAIL_UNLINK") && path &&
        strstr(path, ".jash-heredoc-clean-")) {
        errno = EIO;
        return -1;
    }
    if (getenv("SNAP_FAIL_UNLINK") && path &&
        strstr(path, ".jash-snapshot.recover.")) {
        errno = EIO;
        return -1;
    }
    return real_unlinkat(dirfd, path, flags);
}

int unlink(const char *path) {
    static int (*real_unlink)(const char *) = 0;
    if (!real_unlink) real_unlink = dlsym(RTLD_NEXT, "unlink");
    if (getenv("HEREDOC_FAIL_UNLINK") && path &&
        strstr(path, "jash_heredoc_")) {
        errno = EIO;
        return -1;
    }
    return real_unlink(path);
}

int renameat2(int olddirfd, const char *oldpath, int newdirfd,
              const char *newpath, unsigned int flags) {
    static int (*real_renameat2)(int, const char *, int, const char *, unsigned int) = 0;
    static int swapped = 0;
    if (!real_renameat2) real_renameat2 = dlsym(RTLD_NEXT, "renameat2");
    if (!swapped && getenv("HEREDOC_FAIL_REPLACE") && newpath &&
        strstr(newpath, ".jash-heredoc-clean-")) {
        char proc[64], parent[PATH_MAX], original[PATH_MAX];
        snprintf(proc, sizeof(proc), "/proc/self/fd/%d", olddirfd);
        ssize_t n = readlink(proc, parent, sizeof(parent) - 1);
        if (n >= 0) {
            parent[n] = '\0';
            if (snprintf(original, sizeof(original), "%s/%s", parent, oldpath) <
                (int)sizeof(original) &&
                rename(original, getenv("HEREDOC_SAVED")) == 0 &&
                rename(getenv("HEREDOC_REPLACEMENT"), original) == 0)
                swapped = 1;
        }
    }
    return real_renameat2(olddirfd, oldpath, newdirfd, newpath, flags);
}

int fstatat(int dirfd, const char *path, struct stat *st, int flags) {
    static int (*real_fstatat)(int, const char *, struct stat *, int) = 0;
    static int snapshot_validation_failed = 0;
    if (!real_fstatat) real_fstatat = dlsym(RTLD_NEXT, "fstatat");
    int rc = real_fstatat(dirfd, path, st, flags);
    const char *target = getenv("SNAP_TARGET");
    const char *base = target ? strrchr(target, '/') : 0;
    if (!snapshot_validation_failed && rc == 0 &&
        getenv("SNAP_FAIL_NEW_VALIDATE") && path && base &&
        strcmp(path, base + 1) == 0) {
        snapshot_validation_failed = 1;
        errno = EIO;
        return -1;
    }
    return rc;
}
EOF_C
cc -shared -fPIC -o "$T/io_fault.so" "$T/io_fault.c" -ldl || exit 2

# Drive the actual interactive path: replacement grows beyond line[1024].
cat >"$T/heredoc_pty.py" <<'EOF_PY'
import os, pty, re, select, signal, sys, termios, time
jash = sys.argv[1]
pid, fd = pty.fork()
if pid == 0:
    os.environ.update(JASH_QUIET="1", JASH_SIGIL="0", JASH_NUDGE="0")
    os.execv(jash, ["jash"])

def drain(seconds):
    data = b""
    end = time.time() + seconds
    while time.time() < end:
        ready, _, _ = select.select([fd], [], [], 0.1)
        if ready:
            try:
                data += os.read(fd, 65536)
            except OSError:
                break
    return data

out = drain(0.5)
attrs = termios.tcgetattr(fd)
attrs[3] &= ~termios.ECHO
termios.tcsetattr(fd, termios.TCSANOW, attrs)
for line in ("cat" + " " * 1000 + "<<EOF\n", "body\n", "EOF\n", "echo AFTER_$?\n"):
    os.write(fd, line.encode())
    out += drain(0.4)
end = time.time() + 8
while time.time() < end and not re.search(rb"AFTER_[0-9]+", out):
    out += drain(0.2)
try:
    os.kill(pid, signal.SIGKILL)
    os.waitpid(pid, 0)
except OSError:
    pass
sys.stdout.buffer.write(out)
EOF_PY

pty_out="$(python3 "$T/heredoc_pty.py" "$J" 2>&1 || true)"
if ! printf '%s' "$pty_out" | grep -q 'heredoc: rewritten command too long'; then
    fail "oversized interactive heredoc rewrite fails explicitly" "missing explicit error"
elif ! printf '%s' "$pty_out" | grep -q 'AFTER_2'; then
    fail "oversized interactive heredoc rewrite fails explicitly" "status was not 2 or REPL did not recover"
else
    pass "oversized interactive heredoc rewrite fails explicitly and REPL recovers"
fi

cat >"$T/heredoc_fault_pty.py" <<'EOF_PY'
import os, pty, re, select, signal, sys, time
jash, preload, mode, tmpdir = sys.argv[1:]
if mode == "replace":
    with open(os.path.join(tmpdir, "heredoc-replacement"), "w") as f:
        f.write("REPLACEMENT-INODE")
pid, fd = pty.fork()
if pid == 0:
    env = dict(JASH_QUIET="1", JASH_SIGIL="0", JASH_NUDGE="0",
               LD_PRELOAD=preload, HOME=tmpdir, TMPDIR=tmpdir)
    env["HEREDOC_FAIL_" + mode.upper()] = "1"
    if mode == "replace":
        env["HEREDOC_REPLACEMENT"] = os.path.join(tmpdir, "heredoc-replacement")
        env["HEREDOC_SAVED"] = os.path.join(tmpdir, "heredoc-original-saved")
    os.environ.update(env)
    os.execv(jash, ["jash"])

def drain(seconds):
    data = b""
    end = time.time() + seconds
    while time.time() < end:
        ready, _, _ = select.select([fd], [], [], 0.1)
        if ready:
            try: data += os.read(fd, 65536)
            except OSError: break
    return data

out = drain(0.5)
os.write(fd, b"read HDVALUE <<EOF\n")
command_end = time.time() + 8
while time.time() < command_end and not re.search(rb"read HDVALUE <<EOF\r?\n", out):
    out += drain(0.1)
os.write(fd, b"PARTIAL_SENTINEL\nEOF\n")
fault_end = time.time() + 8
while time.time() < fault_end and b"heredoc:" not in out:
    out += drain(0.1)
os.write(fd, b"echo RESULT_${HDVALUE-unset}_$?\n")
end = time.time() + 8
while time.time() < end and not re.search(rb"RESULT_[^\r\n ]+_[0-9]+", out):
    out += drain(0.2)
try:
    os.kill(pid, signal.SIGKILL)
    os.waitpid(pid, 0)
except OSError:
    pass
sys.stdout.buffer.write(out)
EOF_PY

HEREDOC_FAULT_TMP="$T/heredoc-fault-tmp"
mkdir -m 700 "$HEREDOC_FAULT_TMP"
for mode in dup write close; do
    fault_out="$(python3 "$T/heredoc_fault_pty.py" "$J" "$T/io_fault.so" \
        "$mode" "$HEREDOC_FAULT_TMP" 2>&1 || true)"
    if ! printf '%s' "$fault_out" | grep -q 'heredoc: failed to materialize body'; then
        fail "interactive heredoc $mode failure refuses execution" "missing explicit error"
    elif ! printf '%s' "$fault_out" | grep -q 'RESULT_unset_2'; then
        fail "interactive heredoc $mode failure refuses execution" \
             "consumer ran or status was not 2"
    elif compgen -G "$HEREDOC_FAULT_TMP/jash_heredoc_*" >/dev/null; then
        fail "interactive heredoc $mode failure refuses execution" \
             "temporary body remained named"
    else
        pass "interactive heredoc $mode failure never executes partial temp content"
    fi
done

# A consumer may complete before cleanup fails. That failure overrides the
# ordinary command status with 2, and the still-open inode is scrubbed so the
# named file cannot retain the sensitive body.
HEREDOC_UNLINK_TMP="$T/heredoc-unlink-tmp"
mkdir -m 700 "$HEREDOC_UNLINK_TMP"
unlink_out="$(python3 "$T/heredoc_fault_pty.py" "$J" "$T/io_fault.so" \
    unlink "$HEREDOC_UNLINK_TMP" 2>&1 || true)"
unlink_sensitive=0
for leaked in "$HEREDOC_UNLINK_TMP"/jash_heredoc_* \
              "$HEREDOC_UNLINK_TMP"/.jash-heredoc-clean-*; do
    [ -e "$leaked" ] || continue
    if grep -q 'PARTIAL_SENTINEL' "$leaked"; then unlink_sensitive=1; fi
done
if printf '%s' "$unlink_out" | grep -q 'heredoc: cleanup failed; body scrubbed; status 2' && \
   printf '%s' "$unlink_out" | grep -q 'RESULT_PARTIAL_SENTINEL_2' && \
   [ "$unlink_sensitive" -eq 0 ]; then
    pass "interactive heredoc unlink failure is explicit and scrubs sensitive content"
else
    fail "interactive heredoc unlink failure is explicit and scrubs sensitive content" \
         "sensitive=$unlink_sensitive output=[$unlink_out]"
fi

HEREDOC_REPLACE_TMP="$T/heredoc-replace-tmp"
mkdir -m 700 "$HEREDOC_REPLACE_TMP"
replace_out="$(python3 "$T/heredoc_fault_pty.py" "$J" "$T/io_fault.so" \
    replace "$HEREDOC_REPLACE_TMP" 2>&1 || true)"
replacement_survived=0
for candidate in "$HEREDOC_REPLACE_TMP"/jash_heredoc_*; do
    [ -e "$candidate" ] || continue
    if [ "$(cat "$candidate")" = REPLACEMENT-INODE ]; then
        replacement_survived=1
    fi
done
if printf '%s' "$replace_out" | grep -q 'RESULT_PARTIAL_SENTINEL_2' &&
   [ "$replacement_survived" -eq 1 ] &&
   [ -e "$HEREDOC_REPLACE_TMP/heredoc-original-saved" ] &&
   [ ! -s "$HEREDOC_REPLACE_TMP/heredoc-original-saved" ]; then
    pass "interactive heredoc cleanup preserves replacement and scrubs owned inode"
else
    fail "interactive heredoc cleanup preserves replacement and scrubs owned inode" \
         "replacement=$replacement_survived output=[$replace_out]"
fi

cat >"$T/heredoc_lifecycle_pty.py" <<'EOF_PY'
import os, pty, re, select, signal, sys, time
jash, mode, tmpdir, marker = sys.argv[1:]
pid, fd = pty.fork()
if pid == 0:
    os.environ.update(HOME=tmpdir, TMPDIR=tmpdir, JASH_QUIET="1",
                      JASH_SIGIL="0", JASH_NUDGE="0")
    os.execv(jash, ["jash"])

def drain(seconds):
    data = b""
    end = time.time() + seconds
    while time.time() < end:
        ready, _, _ = select.select([fd], [], [], 0.1)
        if ready:
            try: data += os.read(fd, 65536)
            except OSError: break
    return data

out = drain(0.5)
if mode == "capture":
    command = f"touch {marker} <<EOF\n"
    os.write(fd, command.encode())
    command_end = time.time() + 8
    while time.time() < command_end and not re.search(rb"touch .* <<EOF\r?\n", out):
        out += drain(0.1)
    os.write(fd, b"PARTIAL_BODY\n")
    body_end = time.time() + 8
    while time.time() < body_end and b"PARTIAL_BODY" not in out:
        out += drain(0.1)
    for unused in range(3):
        os.write(fd, b"\x04")
        error_end = time.time() + 2
        while time.time() < error_end and b"heredoc: failed to capture body" not in out:
            out += drain(0.1)
        if b"heredoc: failed to capture body" in out:
            break
    os.write(fd, b"echo CAPTURE_$?\n")
    wanted = rb"CAPTURE_[0-9]+"
else:
    os.write(fd, b"cat <<EOF\n")
    command_end = time.time() + 8
    while time.time() < command_end and not re.search(rb"cat <<EOF\r?\n", out):
        out += drain(0.1)
    os.write(fd, b"CLEANUP_SENTINEL\nEOF\n")
    first_end = time.time() + 8
    while time.time() < first_end and out.count(b"CLEANUP_SENTINEL") < 2:
        out += drain(0.1)
    os.write(fd, b"false <<EOF\n")
    second_end = time.time() + 8
    while time.time() < second_end and not re.search(rb"false <<EOF\r?\n", out):
        out += drain(0.1)
    os.write(fd, b"PRIVATE_BODY\nEOF\n")
    cleanup_end = time.time() + 8
    while time.time() < cleanup_end:
        out += drain(0.1)
        if not any(name.startswith("jash_heredoc_") or
                   name.startswith(".jash-heredoc-clean-")
                   for name in os.listdir(tmpdir)):
            break
    os.write(fd, b"echo LIFECYCLE_$?\n")
    wanted = rb"LIFECYCLE_[0-9]+"
end = time.time() + 8
while time.time() < end and not re.search(wanted, out):
    out += drain(0.2)
try:
    os.kill(pid, signal.SIGKILL)
    os.waitpid(pid, 0)
except OSError:
    pass
sys.stdout.buffer.write(out)
EOF_PY

# EOF before the delimiter is a capture failure, not permission to evaluate the
# consumer against an absent or partial body.
CAPTURE_TMP="$T/heredoc-capture-tmp"
mkdir -m 700 "$CAPTURE_TMP"
capture_marker="$T/CAPTURE-CONSUMER-RAN"
capture_out="$(python3 "$T/heredoc_lifecycle_pty.py" \
    "$J" capture "$CAPTURE_TMP" "$capture_marker" 2>&1 || true)"
if [ ! -e "$capture_marker" ] && \
   printf '%s' "$capture_out" | grep -q 'heredoc: failed to capture body' && \
   printf '%s' "$capture_out" | grep -q 'CAPTURE_2'; then
    pass "interactive heredoc EOF capture failure refuses the consumer"
else
    fail "interactive heredoc EOF capture failure refuses the consumer" \
         "consumer=$([ -e "$capture_marker" ] && echo ran || echo absent) output=[$capture_out]"
fi

# Materialized bodies are command-scoped capabilities: both successful and
# failing consumers must release every temp path before the next prompt.
CLEANUP_TMP="$T/heredoc-cleanup-tmp"
mkdir -m 700 "$CLEANUP_TMP"
cleanup_out="$(python3 "$T/heredoc_lifecycle_pty.py" \
    "$J" cleanup "$CLEANUP_TMP" unused 2>&1 || true)"
cleanup_leak=0
for leaked in "$CLEANUP_TMP"/jash_heredoc_*; do
    [ -e "$leaked" ] || continue
    cleanup_leak=1
done
if [ "$cleanup_leak" -eq 0 ] && \
   printf '%s' "$cleanup_out" | grep -q 'CLEANUP_SENTINEL' && \
   printf '%s' "$cleanup_out" | grep -q 'LIFECYCLE_1'; then
    pass "interactive heredoc temps are removed after success and failure"
else
    fail "interactive heredoc temps are removed after success and failure" \
         "leaked=$cleanup_leak output=[$cleanup_out]"
fi

# Snapshot argv must preserve spaces, quotes, backslashes, and empty arguments.
SNAP_HOME="$T/snap-home"
mkdir -m 700 "$SNAP_HOME" "$SNAP_HOME/.jash"
cat >"$T/restorable.c" <<EOF_C
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
int main(int argc, char **argv) {
    if (argc < 2) return 2;
    int fd = open(argv[1], O_WRONLY|O_CREAT|O_TRUNC, 0600);
    if (fd < 0) return 3;
    for (int i = 2; i < argc; i++) dprintf(fd, "<%s>\n", argv[i]);
    close(fd);
    sleep(20);
    return 0;
}
EOF_C
cc -o "$T/restorable" "$T/restorable.c" || exit 2
"$T/restorable" "$T/restored" "alpha beta" 'quote"slash\' "" & root_pid=$!
PIDS="$PIDS $root_pid"
for unused in $(seq 1 100); do [ -e "$T/restored" ] && break; sleep 0.01; done
printf '<alpha beta>\n<quote"slash\\>\n<>\n' >"$T/expected-args"
SNAP="$T/custom-output.json"
HOME="$SNAP_HOME" JASH_SIGIL=0 JASH_QUIET=1 "$J" -c \
    "proc snapshot $root_pid --output $SNAP" >/dev/null 2>&1
snap_rc=$?
rm -f "$T/restored"
HOME="$SNAP_HOME" JASH_SIGIL=0 JASH_QUIET=1 "$J" -c \
    "proc restore $SNAP" >"$T/restore.out" 2>&1
restore_rc=$?
restore_out="$(cat "$T/restore.out")"
restored_pid="$(printf '%s' "$restore_out" | sed -n 's/.*started pid \([0-9][0-9]*\).*/\1/p')"
[ -n "$restored_pid" ] && PIDS="$PIDS $restored_pid"
for unused in $(seq 1 100); do [ -e "$T/restored" ] && break; sleep 0.01; done
if [ "$snap_rc" -eq 0 ] && [ "$restore_rc" -eq 0 ] && \
   cmp -s "$T/expected-args" "$T/restored"; then
    pass "proc snapshot argv and arbitrary output path round-trip exactly"
else
    fail "proc snapshot argv and arbitrary output path round-trip" \
         "snapshot_rc=$snap_rc restore_rc=$restore_rc output=[$restore_out] args=[$(cat "$T/restored" 2>/dev/null || true)]"
fi

# Oversized process state must fail instead of publishing a snapshot with a
# silently shortened environment or fd table.
cat >"$T/large-state.c" <<'EOF_C'
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
int main(int argc, char **argv) {
    if (argc != 3) return 2;
    if (strcmp(argv[1], "fds") == 0)
        for (int i = 0; i < 256; i++) (void)open("/dev/null", O_RDONLY);
    int ready = open(argv[2], O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (ready < 0) return 3;
    close(ready);
    sleep(30);
    return 0;
}
EOF_C
cc -o "$T/large-state" "$T/large-state.c" || exit 2
cat >"$T/launch-large-env.py" <<'EOF_PY'
import os, subprocess, sys
helper, ready = sys.argv[1:]
env = {"SNAP_LARGE": "\\" * 16360}
p = subprocess.Popen([helper, "env", ready], env=env,
                     stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                     stderr=subprocess.DEVNULL, close_fds=True)
print(p.pid)
EOF_PY
large_env_pid="$(python3 "$T/launch-large-env.py" "$T/large-state" "$T/large-env-ready")"
"$T/large-state" fds "$T/large-fds-ready" & large_fds_pid=$!
PIDS="$PIDS $large_env_pid $large_fds_pid"
for unused in $(seq 1 100); do
    [ -e "$T/large-env-ready" ] && [ -e "$T/large-fds-ready" ] && break
    sleep 0.01
done
LARGE_ENV_SNAP="$T/large-env.json"
LARGE_FDS_SNAP="$T/large-fds.json"
HOME="$SNAP_HOME" JASH_SIGIL=0 JASH_QUIET=1 "$J" -c \
    "proc snapshot $large_env_pid --output $LARGE_ENV_SNAP" >/dev/null 2>&1
large_env_rc=$?
HOME="$SNAP_HOME" JASH_SIGIL=0 JASH_QUIET=1 "$J" -c \
    "proc snapshot $large_fds_pid --output $LARGE_FDS_SNAP" >/dev/null 2>&1
large_fds_rc=$?
if [ "$large_env_rc" -ne 0 ] && [ "$large_fds_rc" -ne 0 ] && \
   [ ! -e "$LARGE_ENV_SNAP" ] && [ ! -e "$LARGE_FDS_SNAP" ]; then
    pass "proc snapshot rejects truncated environment and fd state"
else
    fail "proc snapshot rejects truncated environment and fd state" \
         "env_rc=$large_env_rc env_file=$([ -e "$LARGE_ENV_SNAP" ] && echo yes || echo no) fd_rc=$large_fds_rc fd_file=$([ -e "$LARGE_FDS_SNAP" ] && echo yes || echo no)"
fi

# Version-1 cmdline snapshots remain accepted without guessing argument splits.
cat >"$T/legacy.c" <<EOF_C
#include <fcntl.h>
#include <unistd.h>
int main(void) { int fd = open("$T/legacy-restored", O_WRONLY|O_CREAT, 0600); if (fd >= 0) close(fd); return 0; }
EOF_C
cc -o "$T/legacy" "$T/legacy.c" || exit 2
printf '{"version":1,"root_pid":1,"tree":{"pid":1,"cmdline":"%s ","children":[]}}' \
    "$T/legacy" >"$T/legacy.json"
chmod 0600 "$T/legacy.json"
if HOME="$SNAP_HOME" JASH_SIGIL=0 JASH_QUIET=1 "$J" -c \
       "proc restore $T/legacy.json" >/dev/null 2>&1; then
    legacy_rc=0
else
    legacy_rc=$?
fi
for unused in $(seq 1 100); do [ -e "$T/legacy-restored" ] && break; sleep 0.01; done
if [ "$legacy_rc" -eq 0 ] && [ -e "$T/legacy-restored" ]; then
    pass "legacy cmdline snapshot remains restorable"
else
    fail "legacy cmdline snapshot remains restorable" "rc=$legacy_rc"
fi

# A short Unicode escape must be rejected before the parser reads four digits,
# and malformed snapshot data must never reach exec.
cat >"$T/unicode-exec.c" <<EOF_C
#include <fcntl.h>
#include <unistd.h>
int main(void) { int fd = open("$T/UNICODE-PWNED", O_WRONLY|O_CREAT, 0600); if (fd >= 0) close(fd); return 0; }
EOF_C
cc -o "$T/unicode-exec" "$T/unicode-exec.c" || exit 2
unicode_ok=1
for suffix in '' 1 12 123; do
    malformed="$T/truncated-unicode-${suffix:-none}.json"
    printf '%s%s%s' '{"version":2,"timestamp":"2026-08-27T00:00:00","root_pid":1,"tree":{"pid":1,"argv":["' \
        "$T/unicode-exec" '","\u'"$suffix" >"$malformed"
    chmod 0600 "$malformed"
    rm -f "$T/UNICODE-PWNED"
    if HOME="$SNAP_HOME" JASH_SIGIL=0 JASH_QUIET=1 "$J" -c \
           "proc restore $malformed" >/dev/null 2>&1 || [ -e "$T/UNICODE-PWNED" ]; then
        unicode_ok=0
    fi
done
if [ "$unicode_ok" -eq 1 ]; then
    pass "proc restore rejects truncated Unicode escapes without execution"
else
    fail "proc restore rejects truncated Unicode escapes" "malformed argv reached success or execution"
fi

# Version-2 restore validates the complete generated document before forking.
# Every truncation below occurs after a complete, executable argv array.
cat >"$T/document-exec.c" <<EOF_C
#include <fcntl.h>
#include <unistd.h>
int main(void) { int fd = open("$T/DOCUMENT-EXECUTED", O_WRONLY|O_CREAT, 0600); if (fd >= 0) close(fd); return 0; }
EOF_C
cc -o "$T/document-exec" "$T/document-exec.c" || exit 2
doc_prefix='{"version":2,"timestamp":"2026-08-27T00:00:00","root_pid":1,"tree":{"pid":1,"argv":["'"$T/document-exec"'"]'
printf '%s%s' "$doc_prefix" ',"children":[]}}' >"$T/document-complete.json"
chmod 0600 "$T/document-complete.json"
rm -f "$T/DOCUMENT-EXECUTED"
if HOME="$SNAP_HOME" JASH_SIGIL=0 JASH_QUIET=1 "$J" -c \
       "proc restore $T/document-complete.json" >/dev/null 2>&1; then
    document_complete_rc=0
else
    document_complete_rc=$?
fi
for unused in $(seq 1 100); do [ -e "$T/DOCUMENT-EXECUTED" ] && break; sleep 0.01; done
if [ "$document_complete_rc" -eq 0 ] && [ -e "$T/DOCUMENT-EXECUTED" ]; then
    pass "proc restore accepts a complete version-2 document"
else
    fail "proc restore accepts a complete version-2 document" \
         "rc=$document_complete_rc executed=$([ -e "$T/DOCUMENT-EXECUTED" ] && echo yes || echo no)"
fi

document_matrix_ok=1
matrix_i=0
for suffix in '' ',' ',"children"' ',"children":' ',"children":[' \
              ',"children":[]' ',"children":[]}' ',"children":[]}}x'; do
    matrix_i=$((matrix_i + 1))
    malformed="$T/document-truncated-$matrix_i.json"
    printf '%s%s' "$doc_prefix" "$suffix" >"$malformed"
    chmod 0600 "$malformed"
    rm -f "$T/DOCUMENT-EXECUTED"
    if HOME="$SNAP_HOME" JASH_SIGIL=0 JASH_QUIET=1 "$J" -c \
           "proc restore $malformed" >/dev/null 2>&1; then
        document_matrix_ok=0
    fi
    for unused in $(seq 1 20); do [ -e "$T/DOCUMENT-EXECUTED" ] && break; sleep 0.01; done
    [ ! -e "$T/DOCUMENT-EXECUTED" ] || document_matrix_ok=0
done
if [ "$document_matrix_ok" -eq 1 ]; then
    pass "proc restore rejects complete argv with truncated or trailing version-2 JSON"
else
    fail "proc restore rejects complete argv with truncated or trailing version-2 JSON" \
         "a malformed document returned success or executed argv"
fi

# Failed snapshot persistence must leave an existing valid snapshot untouched.
ATOMIC_SNAP="$T/atomic-output.json"
cp "$SNAP" "$ATOMIC_SNAP"
before_snapshot="$(cat "$ATOMIC_SNAP")"
if HOME="$SNAP_HOME" JASH_SIGIL=0 JASH_QUIET=1 LD_PRELOAD="$T/io_fault.so" \
       SNAP_FAIL_WRITE=1 SNAP_TARGET="$ATOMIC_SNAP" "$J" -c \
       "proc snapshot $root_pid --output $ATOMIC_SNAP" >/dev/null 2>&1; then
    atomic_rc=0
else
    atomic_rc=$?
fi
if [ "$atomic_rc" -ne 0 ] && [ "$(cat "$ATOMIC_SNAP")" = "$before_snapshot" ]; then
    pass "proc snapshot write failure preserves the previous valid file"
else
    fail "proc snapshot write failure preserves the previous valid file" \
         "rc=$atomic_rc previous snapshot changed"
fi

snapshot_prior_recoverable() {
    local target=$1 marker=$2 artifact
    [ "$(cat "$target" 2>/dev/null || true)" = "$marker" ] && return 0
    for artifact in "$T"/.jash-snapshot.recover.*; do
        [ -e "$artifact" ] || continue
        [ "$(cat "$artifact" 2>/dev/null || true)" = "$marker" ] && return 0
    done
    return 1
}

# Publication is not committed until the exchanged directory entry is synced.
FSYNC_SNAP="$T/fsync-output.json"
printf 'PRIOR-FSYNC-SNAPSHOT' >"$FSYNC_SNAP"
chmod 0600 "$FSYNC_SNAP"
if HOME="$SNAP_HOME" JASH_SIGIL=0 JASH_QUIET=1 LD_PRELOAD="$T/io_fault.so" \
       SNAP_FAIL_DIR_FSYNC=1 SNAP_DIR="$T" "$J" -c \
       "proc snapshot $root_pid --output $FSYNC_SNAP" >/dev/null 2>&1; then
    fsync_rc=0
else
    fsync_rc=$?
fi
if [ "$fsync_rc" -ne 0 ] && snapshot_prior_recoverable \
       "$FSYNC_SNAP" PRIOR-FSYNC-SNAPSHOT; then
    pass "proc snapshot preserves prior data when directory fsync fails"
else
    fail "proc snapshot preserves prior data when directory fsync fails" \
         "rc=$fsync_rc target=[$(cat "$FSYNC_SNAP" 2>/dev/null || true)]"
fi

# A failed first publication has no prior target to preserve. Validation and
# directory-sync faults must move the inode back out of the requested target.
for mode in validate fsync; do
    NEW_SNAP="$T/new-$mode-output.json"
    rm -f "$NEW_SNAP"
    case $mode in
        validate) fault_env="SNAP_FAIL_NEW_VALIDATE=1 SNAP_TARGET=$NEW_SNAP" ;;
        fsync) fault_env="SNAP_FAIL_DIR_FSYNC=1 SNAP_DIR=$T" ;;
    esac
    if env HOME="$SNAP_HOME" JASH_SIGIL=0 JASH_QUIET=1 \
           LD_PRELOAD="$T/io_fault.so" $fault_env "$J" -c \
           "proc snapshot $root_pid --output $NEW_SNAP" >/dev/null 2>&1; then
        new_rc=0
    else
        new_rc=$?
    fi
    if [ "$new_rc" -ne 0 ] && [ ! -e "$NEW_SNAP" ]; then
        pass "proc snapshot new-file $mode failure leaves no failed visible target"
    else
        fail "proc snapshot new-file $mode failure leaves no failed visible target" \
             "rc=$new_rc target=$([ -e "$NEW_SNAP" ] && echo visible || echo absent)"
    fi
done

# Once publication is durable, failed old-snapshot cleanup returns nonzero and
# leaves the old inode under the explicit recovery name.
UNLINK_SNAP="$T/unlink-output.json"
printf 'PRIOR-UNLINK-SNAPSHOT' >"$UNLINK_SNAP"
chmod 0600 "$UNLINK_SNAP"
unlink_out="$(HOME="$SNAP_HOME" JASH_SIGIL=0 JASH_QUIET=1 \
       LD_PRELOAD="$T/io_fault.so" SNAP_FAIL_UNLINK=1 "$J" -c \
       "proc snapshot $root_pid --output $UNLINK_SNAP" 2>&1)"
unlink_rc=$?
unlink_json=0
python3 -c 'import json,sys; json.load(open(sys.argv[1]))' "$UNLINK_SNAP" \
    >/dev/null 2>&1 && unlink_json=1
unlink_recovery=""
for artifact in "$T"/.jash-snapshot.recover.*; do
    [ -e "$artifact" ] || continue
    if [ "$(cat "$artifact" 2>/dev/null || true)" = PRIOR-UNLINK-SNAPSHOT ]; then
        unlink_recovery=$artifact
        break
    fi
done
if [ "$unlink_rc" -ne 0 ] && [ "$unlink_json" -eq 1 ] && \
   [ -n "$unlink_recovery" ] && [[ "$unlink_out" == *"$unlink_recovery"* ]]; then
    pass "proc snapshot leaves prior recovery artifact when cleanup unlink fails"
else
    fail "proc snapshot leaves prior recovery artifact when cleanup unlink fails" \
         "rc=$unlink_rc new_json=$unlink_json recovery=$unlink_recovery output=[$unlink_out]"
fi

# Successful unlink is not durable cleanup until the containing directory is
# synced again. The injected second directory fsync proves that post-unlink
# barrier is present and its failure is propagated.
CLEANUP_SYNC_SNAP="$T/cleanup-sync-output.json"
printf 'PRIOR-CLEANUP-SYNC' >"$CLEANUP_SYNC_SNAP"
chmod 0600 "$CLEANUP_SYNC_SNAP"
cleanup_sync_out="$(HOME="$SNAP_HOME" JASH_SIGIL=0 JASH_QUIET=1 \
    LD_PRELOAD="$T/io_fault.so" SNAP_FAIL_CLEANUP_FSYNC=1 SNAP_DIR="$T" \
    "$J" -c "proc snapshot $root_pid --output $CLEANUP_SYNC_SNAP" 2>&1)"
cleanup_sync_rc=$?
cleanup_sync_json=0
python3 -c 'import json,sys; json.load(open(sys.argv[1]))' "$CLEANUP_SYNC_SNAP" \
    >/dev/null 2>&1 && cleanup_sync_json=1
cleanup_sync_prior=0
snapshot_prior_recoverable "$CLEANUP_SYNC_SNAP" PRIOR-CLEANUP-SYNC && \
    cleanup_sync_prior=1
if [ "$cleanup_sync_rc" -ne 0 ] && [ "$cleanup_sync_json" -eq 1 ] && \
   [ "$cleanup_sync_prior" -eq 0 ]; then
    pass "proc snapshot fsyncs directory after recovery unlink"
else
    fail "proc snapshot fsyncs directory after recovery unlink" \
         "rc=$cleanup_sync_rc new_json=$cleanup_sync_json prior=$cleanup_sync_prior output=[$cleanup_sync_out]"
fi

# Swap the validated destination immediately before commit. Overwrite must
# exchange, detect the unexpected old inode, and roll it back into place.
REPLACE_SNAP="$T/replace-output.json"
REPLACEMENT="$T/replacement.json"
cp "$SNAP" "$REPLACE_SNAP"
printf 'REPLACEMENT-INODE' >"$REPLACEMENT"
chmod 0600 "$REPLACE_SNAP" "$REPLACEMENT"
cat >"$T/commit_swap.c" <<'EOF_C'
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

int renameat2(int olddirfd, const char *oldpath, int newdirfd,
              const char *newpath, unsigned flags) {
    static int (*real_renameat2)(int, const char *, int, const char *, unsigned) = 0;
    static int swapped = 0;
    if (!real_renameat2) real_renameat2 = dlsym(RTLD_NEXT, "renameat2");
    if (!swapped++)
        rename(getenv("SNAP_COMMIT_REPLACEMENT"), getenv("SNAP_COMMIT_TARGET"));
    return real_renameat2(olddirfd, oldpath, newdirfd, newpath, flags);
}
EOF_C
cc -shared -fPIC -o "$T/commit_swap.so" "$T/commit_swap.c" -ldl || exit 2
if HOME="$SNAP_HOME" JASH_SIGIL=0 JASH_QUIET=1 LD_PRELOAD="$T/commit_swap.so" \
       SNAP_COMMIT_TARGET="$REPLACE_SNAP" SNAP_COMMIT_REPLACEMENT="$REPLACEMENT" \
       "$J" -c "proc snapshot $root_pid --output $REPLACE_SNAP" >/dev/null 2>&1; then
    replace_rc=0
else
    replace_rc=$?
fi
if [ "$replace_rc" -ne 0 ] && [ "$(cat "$REPLACE_SNAP")" = REPLACEMENT-INODE ] && \
   [ ! -e "$REPLACEMENT" ]; then
    pass "proc snapshot rolls back a destination replaced before commit"
else
    fail "proc snapshot rolls back a destination replaced before commit" \
         "rc=$replace_rc target=[$(cat "$REPLACE_SNAP" 2>/dev/null || true)] replacement_exists=$([ -e "$REPLACEMENT" ] && echo yes || echo no)"
fi

# Snapshot creation must not truncate a symlink target.
printf 'KEEP' >"$T/snapshot-victim"
ln -s "$T/snapshot-victim" "$T/snapshot-link"
if HOME="$SNAP_HOME" JASH_SIGIL=0 JASH_QUIET=1 "$J" -c \
       "proc snapshot $root_pid --output $T/snapshot-link" >/dev/null 2>&1; then
    link_rc=0
else
    link_rc=$?
fi
if [ "$link_rc" -ne 0 ] && [ "$(cat "$T/snapshot-victim")" = KEEP ]; then
    pass "proc snapshot refuses a symlink output without clobbering its target"
else
    fail "proc snapshot refuses a symlink output" \
         "rc=$link_rc victim=[$(cat "$T/snapshot-victim")]"
fi

# Restore rejects snapshots that other users could modify.
chmod 0644 "$SNAP"
rm -f "$T/restored"
if HOME="$SNAP_HOME" JASH_SIGIL=0 JASH_QUIET=1 "$J" -c \
       "proc restore $SNAP" >/dev/null 2>&1; then
    fail "proc restore rejects group/world-accessible snapshots" "restore returned success"
elif [ -e "$T/restored" ]; then
    fail "proc restore rejects group/world-accessible snapshots" "snapshot was executed"
else
    pass "proc restore rejects group/world-accessible snapshots"
fi
chmod 0600 "$SNAP"

# Force the pathname to resolve to one inode at validation and another at open.
cat >"$T/evil.c" <<EOF_C
#include <fcntl.h>
#include <unistd.h>
int main(void) { int fd = open("$T/PWNED", O_WRONLY|O_CREAT, 0600); if (fd >= 0) close(fd); return 0; }
EOF_C
cc -o "$T/evil" "$T/evil.c" || exit 2
mkdir -m 700 "$SNAP_HOME/.jash/good" "$SNAP_HOME/.jash/evil"
cp "$SNAP" "$SNAP_HOME/.jash/good/snap.json"
printf '{"version":1,"root_pid":1,"tree":{"pid":1,"cmdline":"%s","children":[]}}' \
    "$T/evil" >"$SNAP_HOME/.jash/evil/snap.json"
chmod 0600 "$SNAP_HOME/.jash/good/snap.json" "$SNAP_HOME/.jash/evil/snap.json"
ln -s "$SNAP_HOME/.jash/good" "$SNAP_HOME/.jash/swap"

cat >"$T/swap_open.c" <<'EOF_C'
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void maybe_swap(const char *path, int flags) {
    static int swapped = 0;
    const char *wanted = getenv("SWAP_PATH");
    if (!swapped && wanted && strcmp(path, wanted) == 0 &&
        (flags & O_ACCMODE) == O_RDONLY) {
        swapped = 1;
        unlink(getenv("SWAP_LINK"));
        symlink(getenv("SWAP_TO"), getenv("SWAP_LINK"));
    }
}

int open(const char *path, int flags, ...) {
    static int (*real_open)(const char *, int, ...) = 0;
    if (!real_open) real_open = dlsym(RTLD_NEXT, "open");
    maybe_swap(path, flags);
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode_t mode = (mode_t)va_arg(ap, int);
        va_end(ap);
        return real_open(path, flags, mode);
    }
    return real_open(path, flags);
}

int __open_2(const char *path, int flags) {
    static int (*real_open_2)(const char *, int) = 0;
    if (!real_open_2) real_open_2 = dlsym(RTLD_NEXT, "__open_2");
    maybe_swap(path, flags);
    return real_open_2(path, flags);
}
EOF_C
cc -shared -fPIC -o "$T/swap_open.so" "$T/swap_open.c" -ldl || exit 2
SWAP_PATH="$SNAP_HOME/.jash/swap/snap.json"
rm -f "$T/PWNED"
if HOME="$SNAP_HOME" JASH_SIGIL=0 JASH_QUIET=1 LD_PRELOAD="$T/swap_open.so" \
       SWAP_PATH="$SWAP_PATH" SWAP_LINK="$SNAP_HOME/.jash/swap" \
       SWAP_TO="$SNAP_HOME/.jash/evil" "$J" -c "proc restore $SWAP_PATH" \
       >/dev/null 2>&1; then
    swap_rc=0
else
    swap_rc=$?
fi
for unused in $(seq 1 100); do [ -e "$T/PWNED" ] && break; sleep 0.01; done
if [ "$swap_rc" -ne 0 ] && [ ! -e "$T/PWNED" ]; then
    pass "proc restore rejects an inode swap between validation and open"
else
    fail "proc restore rejects an inode swap" "rc=$swap_rc executed=$([ -e "$T/PWNED" ] && echo yes || echo no)"
fi

printf '== security state: %d passed, %d failed ==\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]
