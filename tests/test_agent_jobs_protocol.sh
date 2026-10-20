#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${JASH_AGENTD_BIN:-$ROOT/build/jash-agentd}"
PYTHON_BIN="$(command -v python3 || command -v python)"
test -x "$BIN"

TMP="${TMPDIR:-/tmp}/jash-agent-jobs.$$"
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT INT TERM

"$PYTHON_BIN" - "$BIN" "$PYTHON_BIN" "$TMP" <<'PY'
import base64
import json
import os
import signal
import stat
import subprocess
import sys
import time

binary, python, root = sys.argv[1:]
verifier = os.path.join(os.path.dirname(binary), "jef_verify")
create_caps = ["effect:exec", "job:create"]


def daemon(name, extra_env=None, pass_fds=()):
    home = os.path.join(root, name)
    os.makedirs(home, exist_ok=True)
    environment = {**os.environ, "HOME": home}
    environment.pop("W68_ENV", None)
    environment.pop("JASH_AGENT_CGROUP_ROOT", None)
    if extra_env:
        environment.update(extra_env)
    proc = subprocess.Popen(
        [binary], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, text=True,
        env=environment,
        pass_fds=pass_fds,
    )
    assert proc.stdin and proc.stdout and proc.stderr
    handshake = json.loads(proc.stdout.readline())
    return proc, handshake, home


def request_raw(proc, body, wire=None):
    encoded = wire if wire is not None else json.dumps(body, separators=(",", ":"))
    proc.stdin.write(encoded + "\n")
    proc.stdin.flush()
    line = proc.stdout.readline()
    assert line, (body, proc.stderr.read())
    return json.loads(line), line


def request(proc, body, wire=None):
    return request_raw(proc, body, wire)[0]


def finish(proc):
    proc.stdin.close()
    extra = [json.loads(line) for line in proc.stdout if line.strip()]
    stderr = proc.stderr.read()
    rc = proc.wait(timeout=20)
    assert rc == 0, (rc, stderr, extra[-3:])
    assert not extra, extra


def job_read(proc, request_id, job_id, stream, offset, limit=65536):
    return request(proc, {
        "id": request_id, "protocol_version": 2, "op": "job.read",
        "job_id": job_id, "stream": stream, "offset": offset,
        "limit": limit, "caps": ["job:read"],
    })


def read_until(proc, job_id, stream, needle, timeout=3, offset=0):
    data = bytearray()
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        row = job_read(proc, f"read-{job_id}-{offset}", job_id, stream, offset, 4096)
        chunk = base64.b64decode(row["data_b64"], validate=True)
        assert row["offset"] == offset and row["bytes"] == len(chunk), row
        data += chunk
        offset = row["next_offset"]
        if needle in data:
            return bytes(data), offset
        time.sleep(0.01)
    raise AssertionError((job_id, stream, bytes(data), needle))


proc, handshake, home = daemon("main")
jobs = handshake["jobs"]
assert handshake["schema_version"] == 18, handshake
assert jobs == {
    "enabled": True,
    "max_jobs": 32,
    "id_scope": "persistent-shared",
    "start_idempotency": "exact-request-persistent",
    "terminal_receipts": True,
    "start_is_terminal": False,
    "session_end": {"policy": "cancel-reap-archive", "result_op": "job.result", "reattach": False},
    "protected_env": ["HOME", "PWD", "JASH_AGENT_CANCEL_FD", "JASH_AGENT_CGROUP_ROOT"],
    "features": {
        "exec": True, "background": True, "poll": True, "wait": True,
        "cancel_term_kill": True,
        "cwd": True, "env": True, "binary_stdin": True,
        "artifact_stdin": True, "terminal_stream_stdin": True,
        "stdin_pipe": True, "stdin_half_close": True,
        "pty": sys.platform.startswith("linux"),
        "pty_resize": sys.platform.startswith("linux"),
        "pty_write": sys.platform.startswith("linux"),
        "paged_base64_read": True, "separate_spools": True,
    },
    "resources": {"backend": "cgroup-v2", "configured": False,
                  "enforcement": "required-per-job-setup", "cpu_semantics": "bandwidth",
                  "swap_max_bytes": 0, "oom_group": True},
}, jobs
expected_job_caps = {
    "effect:exec", "job:create", "job:read", "job:signal",
    "job:stdin:write",
    "store:read", "store:manage",
    "data:create", "data:read", "data:manage",
}
if sys.platform.startswith("linux"):
    expected_job_caps.update({
        "job:pty:create", "job:pty:resize", "job:pty:write",
    })
assert set(handshake["job_capabilities"]) == expected_job_caps
assert handshake["job_limits"] == {
    "jobs": 32, "wait_ms": 30000, "cancel_grace_ms": 5000,
    "read_bytes": 65536, "write_bytes": 16384, "stdin_bytes": 16384,
    "cwd_bytes": 4096, "env_items": 16, "env_bytes": 16384,
    "env_key_bytes": 255, "env_value_bytes": 2047,
    "effective_env_items": 256, "effective_env_bytes": 65536,
    "output_bytes": 67108864,
    "pipeline_stages": 16,
}, handshake["job_limits"]
assert handshake["receipts"]["jobs"] == {
    "schema": "jash.agent-job-receipt.v1",
    "media_type": "application/vnd.jash.agent-job-receipt.v1+json",
    "phases": ["start", "action", "terminal"],
}, handshake["receipts"]
assert jobs["features"]["pty"] == sys.platform.startswith("linux")
assert jobs["features"]["pty_resize"] == sys.platform.startswith("linux")
assert jobs["features"]["pty_write"] == sys.platform.startswith("linux")
assert "data_b64" in handshake["fields"]
assert handshake["pwd_semantics"] == "logical-open-snapshot"

# Explicit pipe mode carries two independently observed JSON-RPC exchanges.
# Protocol half-close is intentionally deferred; the child exits after two
# newline-delimited frames while the runtime owns closure on terminal state.
pipe_code = (
    "import json,os\n"
    "pending=b''\n"
    "for _ in range(2):\n"
    " while b'\\n' not in pending:\n"
    "  chunk=os.read(0,4096)\n"
    "  assert chunk\n"
    "  pending+=chunk\n"
    " line,pending=pending.split(b'\\n',1)\n"
    " request=json.loads(line)\n"
    " response={'jsonrpc':'2.0','id':request['id'],'result':request['method']}\n"
    " os.write(1,json.dumps(response,separators=(',',':')).encode()+b'\\n')\n"
)
pipe_start = request(proc, {
    "id": "stdin-pipe-start", "protocol_version": 2, "op": "job.start",
    "argv": [python, "-c", pipe_code], "background": True,
    "execution_spec_sha256": "sha256:" + "1" * 64,
    "stdin_mode": "pipe", "caps": create_caps,
})
pipe_id = pipe_start["job_id"]
pipe_offset = 0
for index, method in enumerate(("initialize", "tools/list"), 1):
    frame = json.dumps({
        "jsonrpc": "2.0", "id": index, "method": method,
    }, separators=(",", ":")).encode() + b"\n"
    written = request(proc, {
        "id": f"stdin-pipe-write-{index}", "protocol_version": 2,
        "op": "job.write", "job_id": pipe_id,
        "data_b64": base64.b64encode(frame).decode(),
        "caps": ["job:stdin:write"],
    })
    assert written["accepted_bytes"] == len(frame), written
    expected = json.dumps({
        "jsonrpc": "2.0", "id": index, "result": method,
    }, separators=(",", ":")).encode() + b"\n"
    observed, pipe_offset = read_until(
        proc, pipe_id, "stdout", expected, offset=pipe_offset,
    )
    assert observed == expected, observed
pipe_done = request(proc, {
    "id": "stdin-pipe-wait", "protocol_version": 2, "op": "job.wait",
    "job_id": pipe_id, "wait_ms": 2000, "caps": ["job:read"],
})
assert pipe_done["state"] == "done" and pipe_done["rc"] == 0, pipe_done

# Pipe writes remain byte-exact and reject non-canonical base64 before effect.
binary_pipe = request(proc, {
    "id": "binary-pipe-start", "protocol_version": 2, "op": "job.start",
    "argv": [python, "-c",
             "import os;d=b''\nwhile len(d)<3:d+=os.read(0,3-len(d))\nos.write(1,d)"],
    "background": True, "stdin_mode": "pipe", "caps": create_caps,
})
malformed_pipe = request(proc, {
    "id": "binary-pipe-malformed", "protocol_version": 2,
    "op": "job.write", "job_id": binary_pipe["job_id"],
    "data_b64": "AB==", "caps": ["job:stdin:write"],
})
assert "data_b64" in malformed_pipe["error"], malformed_pipe
binary_payload = b"\x00\xff\n"
binary_written = request(proc, {
    "id": "binary-pipe-write", "protocol_version": 2,
    "op": "job.write", "job_id": binary_pipe["job_id"],
    "data_b64": base64.b64encode(binary_payload).decode(),
    "caps": ["job:stdin:write"],
})
assert binary_written["accepted_bytes"] == len(binary_payload), binary_written
binary_seen, _ = read_until(
    proc, binary_pipe["job_id"], "stdout", binary_payload,
)
assert binary_seen == binary_payload, binary_seen

for rejected, expected_error in (
    ({"id": "pipe-pty", "protocol_version": 2, "op": "job.start",
      "argv": ["/bin/true"], "background": True, "stdin_mode": "pipe",
      "pty": True, "caps": create_caps + ["job:pty:create"]}, "pty"),
    ({"id": "pipe-static", "protocol_version": 2, "op": "job.start",
      "argv": ["/bin/true"], "background": True, "stdin_mode": "pipe",
      "stdin_b64": "eA==", "caps": create_caps}, "stdin_b64"),
    ({"id": "pipe-invalid", "protocol_version": 2, "op": "job.start",
      "argv": ["/bin/true"], "background": True, "stdin_mode": "other",
      "caps": create_caps}, "stdin_mode"),
):
    rejection = request(proc, rejected)
    assert expected_error in rejection["error"], (rejected, rejection)

# A non-reading child applies bounded backpressure. Partial acceptance is
# indeterminate and is never replayed; cancellation remains available.
blocked_pipe = request(proc, {
    "id": "blocked-pipe-start", "protocol_version": 2, "op": "job.start",
    "argv": [python, "-c",
             "import os,time;os.write(1,b'PIPE_READY\\n');time.sleep(30)"],
    "background": True, "stdin_mode": "pipe", "caps": create_caps,
})
blocked_pipe_id = blocked_pipe["job_id"]
read_until(proc, blocked_pipe_id, "stdout", b"PIPE_READY")
partial = None
for attempt in range(20):
    body = {
        "id": f"blocked-pipe-write-{attempt}", "protocol_version": 2,
        "op": "job.write", "job_id": blocked_pipe_id, "wait_ms": 10,
        "data_b64": base64.b64encode(b"z" * 10000).decode(),
        "caps": ["job:stdin:write"],
    }
    before = time.monotonic()
    row = request(proc, body)
    assert time.monotonic() - before < 0.5, row
    if row.get("outcome") == "indeterminate":
        partial = row
        break
    assert row["accepted_bytes"] == 10000, row
assert partial is not None and 0 < partial["accepted_bytes"] < 10000, partial
partial_retry = request(proc, body)
assert partial_retry["outcome"] == "indeterminate", partial_retry
assert partial_retry["error"] == "request outcome unknown", partial_retry
blocked_pipe_cancel = request(proc, {
    "id": "blocked-pipe-cancel", "protocol_version": 2,
    "op": "job.cancel", "job_id": blocked_pipe_id, "grace_ms": 10,
    "caps": ["job:signal"],
})
assert blocked_pipe_cancel["state"] == "done", blocked_pipe_cancel

# Shell-free exec and job.start receive isolated context. Relative cwd is
# resolved against the daemon cwd, binary stdin is exact, and neither request
# mutates the daemon's own cwd or environment.
daemon_cwd = os.getcwd()
context_dir = os.path.join(root, "context")
os.makedirs(context_dir)
context_stdin = b"CTX\x00\xff\n"
context_code = (
    "import os; data=b''; raw=open('/proc/self/environ','rb').read().split(b'\\0')\n"
    "while True:\n"
    " chunk=os.read(0,4096)\n"
    " if not chunk: break\n"
    " data+=chunk\n"
    "pwds=[x for x in raw if x.startswith(b'PWD=')]\n"
    "os.write(1,os.getcwd().encode()+b'|'+os.environ['PWD'].encode()+b'|'+"
    "str(len(pwds)).encode()+b'|'+os.environ.get('JASH_AGENT_CANCEL_FD','MISSING').encode()+"
    "b'|'+os.environ['W68_ENV'].encode()+b'|'+data)"
)
context_exec = request(proc, {
    "id": "context-exec", "protocol_version": 2, "op": "exec",
    "argv": [python, "-c", context_code], "cwd": context_dir,
    "env": {"W68_ENV": "ABS"},
    "stdin_b64": base64.b64encode(context_stdin).decode(),
    "caps": create_caps,
})
assert context_exec["state"] == "done" and context_exec["rc"] == 0, context_exec
context_out = job_read(proc, "context-exec-read", context_exec["job_id"],
                       "stdout", 0)
assert base64.b64decode(context_out["data_b64"], validate=True) == (
    context_dir.encode() + b"|" + context_dir.encode() + b"|1|MISSING|ABS|" +
    context_stdin
), context_out
assert context_exec["receipt_phase"] == "terminal", context_exec
assert context_exec["start_receipt_ref"].endswith(".job-start.dsse"), context_exec

relative_context = os.path.relpath(context_dir, daemon_cwd)
context_start = request(proc, {
    "id": "context-start", "protocol_version": 2, "op": "job.start",
    "argv": [python, "-c", context_code], "background": True,
    "cwd": relative_context, "env": {"W68_ENV": "REL"},
    "stdin_b64": base64.b64encode(context_stdin).decode(),
    "caps": create_caps,
})
context_done = request(proc, {
    "id": "context-start-wait", "protocol_version": 2, "op": "job.wait",
    "job_id": context_start["job_id"], "wait_ms": 2000,
    "caps": ["job:read"],
})
context_job_out = job_read(proc, "context-start-read", context_start["job_id"],
                           "stdout", 0)
assert base64.b64decode(context_job_out["data_b64"], validate=True) == (
    context_dir.encode() + b"|" + context_dir.encode() + b"|1|MISSING|REL|" +
    context_stdin
), context_job_out
assert context_done["start_receipt_ref"] == context_start["receipt_ref"], context_done
daemon_pwd = request(proc, {
    "id": "daemon-cwd-unchanged", "argv": ["pwd"], "caps": ["effect:read"],
})
assert daemon_pwd["stdout"].strip() == daemon_cwd, daemon_pwd
daemon_env = request(proc, {
    "id": "daemon-env-unchanged", "argv": [python, "-c",
    "import os;print(os.environ.get('W68_ENV','MISSING'))"],
    "caps": ["effect:exec"],
})
assert daemon_env["stdout"].strip() == "MISSING", daemon_env

# Unknown inherited descriptors and the daemon cancellation transport never
# cross execve, while descriptors 0/1/2 and inherited HOME remain usable.
sentinel_path = os.path.join(root, "daemon-fd-sentinel")
sentinel_fd = os.open(sentinel_path, os.O_RDWR | os.O_CREAT | os.O_TRUNC, 0o600)
cancel_read, cancel_write = os.pipe()
os.set_inheritable(sentinel_fd, True)
os.set_inheritable(cancel_read, True)
fd_proc, _, fd_home = daemon(
    "fd-isolation", {"JASH_AGENT_CANCEL_FD": str(cancel_read)},
    (sentinel_fd, cancel_read),
)
fd_probe = request(fd_proc, {
    "id": "fd-probe", "protocol_version": 2, "op": "exec",
    "argv": [python, "-c",
             "import errno,fcntl,os,sys; fd=int(sys.argv[1]); "
             "assert all(fcntl.fcntl(x,fcntl.F_GETFD)>=0 for x in (0,1,2)); "
             "\ntry: fcntl.fcntl(fd,fcntl.F_GETFD); raise AssertionError('open')"
             "\nexcept OSError as e: assert e.errno==errno.EBADF"
             "\nassert 'JASH_AGENT_CANCEL_FD' not in os.environ"
             "\nassert os.environ['HOME']==sys.argv[2]",
             str(sentinel_fd), fd_home],
    "caps": create_caps,
})
assert fd_probe["rc"] == 0, fd_probe
finish(fd_proc)
os.close(cancel_write)
os.close(cancel_read)
os.close(sentinel_fd)

# Persistent create conflicts are resolved before inherited environment or
# stdin preparation. A resource failure cannot hide an already-owned ID.
reserve_proc, _, _ = daemon("create-reserve-order")
reserve_seed = request(reserve_proc, {
    "id": "reserve-before-env", "protocol_version": 2, "op": "job.start",
    "argv": ["/bin/true"], "background": True, "caps": create_caps,
})
assert reserve_seed["accepted"] is True, reserve_seed
finish(reserve_proc)
reserve_conflict_proc, _, _ = daemon(
    "create-reserve-order", {"JASH_TEST_HUGE_ENV": "x" * 70000})
reserve_conflict = request(reserve_conflict_proc, {
    "id": "reserve-before-env", "protocol_version": 2, "op": "job.start",
    "argv": ["/bin/false"], "background": True, "caps": create_caps,
})
assert reserve_conflict["error"] == "idempotency conflict", reserve_conflict
finish(reserve_conflict_proc)

# Mutation responses survive the daemon that applied them. Replay happens
# from the shared persistent namespace before any live-job lookup.
persist_proc, _, _ = daemon("mutation-persistent")
persist_start = request(persist_proc, {
    "id": "persist-start", "protocol_version": 2, "op": "job.start",
    "argv": [python, "-c", "import time;time.sleep(30)"],
    "background": True, "pty": True,
    "caps": create_caps + ["job:pty:create"],
})
persist_write = {
    "id": "persist-write", "protocol_version": 2, "op": "job.write",
    "job_id": persist_start["job_id"], "data_b64": "AAEC",
    "caps": ["job:pty:write"],
}
persist_resize = {
    "id": "persist-resize", "protocol_version": 2, "op": "job.resize",
    "job_id": persist_start["job_id"], "rows": 31, "cols": 79,
    "caps": ["job:pty:resize"],
}
persist_cancel = {
    "id": "persist-cancel", "protocol_version": 2, "op": "job.cancel",
    "job_id": persist_start["job_id"], "grace_ms": 0,
    "caps": ["job:signal"],
}
persist_rows = []
for body in (persist_write, persist_resize, persist_cancel):
    wire = json.dumps(body, separators=(",", ":"))
    parsed, raw = request_raw(persist_proc, body, wire)
    assert "error" not in parsed, parsed
    persist_rows.append((body, wire, raw))
finish(persist_proc)
persist_replay, _, _ = daemon("mutation-persistent")
for body, wire, expected_raw in persist_rows:
    _, replay_raw = request_raw(persist_replay, body, wire)
    assert replay_raw == expected_raw, (body, replay_raw, expected_raw)
finish(persist_replay)

# Background starts immediately, and retrying the exact request never starts
# a second process. Output is read byte-exact in independent pages.
marker = os.path.join(root, "idempotent-marker")
payload = b"A\x00B\xffC"
code = (
    f"import os,time; open({marker!r},'ab').write(b'x'); "
    f"time.sleep(.35); os.write(1,{payload!r}); os.write(2,b'ERR\\x00')"
)
start = {
    "id": "start-once", "protocol_version": 2, "op": "job.start",
    "argv": [python, "-c", code], "background": True,
    "caps": create_caps,
}
wire = json.dumps(start, separators=(",", ":"))
before = time.monotonic()
started = request(proc, start, wire)
assert time.monotonic() - before < 0.2, started
assert started["accepted"] is True and started["terminal"] is False, started
assert started["receipt_phase"] == "start" and not started["job_id"].isdigit(), started
assert started["receipt_ref"].endswith(".job-start.dsse"), started
replayed = request(proc, start, wire)
assert replayed == started, (started, replayed)
job_id = started["job_id"]

got = request(proc, {
    "id": "get-running", "protocol_version": 2, "op": "job.get",
    "job_id": job_id, "caps": ["job:read"],
})
assert got["state"] in ("running", "done"), got
short = request(proc, {
    "id": "wait-short", "protocol_version": 2, "op": "job.wait",
    "job_id": job_id, "wait_ms": 1, "caps": ["job:read"],
})
assert short["timed_out"] is True and short["terminal"] is False, short
done = request(proc, {
    "id": "wait-done", "protocol_version": 2, "op": "job.wait",
    "job_id": job_id, "wait_ms": 2000, "caps": ["job:read"],
})
assert done["state"] == "done" and done["rc"] == 0, done
assert done["terminal"] is True and done["quiescent"] is True, done
assert done["quiescence_forced"] is False and done["output_limit"] is False, done
assert done["output_limit_bytes"] > 0, done
assert done["receipt_phase"] == "terminal", done
assert done["receipt_ref"].endswith(".job-terminal.dsse"), done
assert done["start_receipt_ref"] == started["receipt_ref"], done
assert done["stdout_ref"] and done["stderr_ref"], done

first = job_read(proc, "page-1", job_id, "stdout", 0, 2)
second = job_read(proc, "page-2", job_id, "stdout", first["next_offset"], 2)
third = job_read(proc, "page-3", job_id, "stdout", second["next_offset"], 2)
decoded = b"".join(base64.b64decode(row["data_b64"], validate=True)
                   for row in (first, second, third))
assert decoded == payload and third["eof"] is True, (decoded, first, second, third)
stderr = job_read(proc, "stderr", job_id, "stderr", 0)
assert base64.b64decode(stderr["data_b64"], validate=True) == b"ERR\x00", stderr
assert open(marker, "rb").read() == b"x"

# Terminal reads are pinned to the immutable CAS inode. A hardlink or a
# same-content replacement is rejected rather than silently trusted.
cas_path = os.path.join(home, ".jash", "overflow", done["stdout_ref"])
cas_link = cas_path + ".link"
os.link(cas_path, cas_link)
assert job_read(proc, "cas-hardlink", job_id, "stdout", 0)["error"] == \
       "terminal CAS unavailable"
os.unlink(cas_link)
cas_replacement = cas_path + ".replacement"
open(cas_replacement, "wb").write(open(cas_path, "rb").read())
os.chmod(cas_replacement, 0o600)
os.replace(cas_replacement, cas_path)
assert job_read(proc, "cas-replaced", job_id, "stdout", 0)["error"] == \
       "terminal CAS identity changed"

# Job creation and synchronous execution reserve one persistent id namespace.
cross_marker = os.path.join(root, "cross-op-once")
cross_id = "cross-op-id"
cross_start = request(proc, {
    "id": cross_id, "protocol_version": 2, "op": "job.start",
    "argv": [python, "-c", f"open({cross_marker!r},'ab').write(b'x')"],
    "background": True, "caps": create_caps,
})
cross_sync = request(proc, {
    "id": cross_id, "argv": [python, "-c",
    f"open({cross_marker!r},'ab').write(b'y')"], "caps": ["effect:exec"],
})
assert cross_sync["error"] == "idempotency conflict", cross_sync
request(proc, {
    "id": "cross-wait", "protocol_version": 2, "op": "job.wait",
    "job_id": cross_start["job_id"], "wait_ms": 2000, "caps": ["job:read"],
})
assert open(cross_marker, "rb").read() == b"x"

reverse_marker = os.path.join(root, "cross-op-reverse")
sync_first = request(proc, {
    "id": "cross-op-reverse-id", "argv": [python, "-c",
    f"open({reverse_marker!r},'ab').write(b's')"], "caps": ["effect:exec"],
})
assert sync_first["rc"] == 0, sync_first
job_second = request(proc, {
    "id": "cross-op-reverse-id", "protocol_version": 2, "op": "job.start",
    "argv": [python, "-c", f"open({reverse_marker!r},'ab').write(b'j')"],
    "background": True, "caps": create_caps,
})
assert job_second["error"] == "idempotency conflict", job_second
assert open(reverse_marker, "rb").read() == b"s"

# V1 stdin is written completely rather than relying on one write(2).
v1_stdin = "v1-stdin-" + "x" * 24000
v1_result = request(proc, {
    "id": "v1-stdin-complete", "argv": ["/bin/cat"],
    "stdin": v1_stdin, "caps": ["effect:exec"],
})
assert v1_result["rc"] == 0 and v1_result["stdout"] == v1_stdin, v1_result

# Once launch succeeds, inability to commit its start receipt makes the
# outcome indeterminate and synchronously cancels/reaps the worker.
receipt_dir = os.path.join(home, ".jash", "agent_receipts")
receipt_failure_marker = os.path.join(root, "receipt-failure-survived")
receipt_dir_saved = receipt_dir + ".saved"
os.rename(receipt_dir, receipt_dir_saved)
open(receipt_dir, "w").write("not a directory")
receipt_failed = request(proc, {
    "id": "receipt-failure", "protocol_version": 2, "op": "job.start",
    "argv": [python, "-c",
             f"import time;time.sleep(.2);open({receipt_failure_marker!r},'w').write('x');time.sleep(30)"],
    "background": True, "caps": create_caps,
})
os.unlink(receipt_dir)
os.rename(receipt_dir_saved, receipt_dir)
assert receipt_failed["outcome"] == "indeterminate", receipt_failed
time.sleep(.3)
assert not os.path.exists(receipt_failure_marker), receipt_failed

spool_dir = os.path.join(home, ".jash", "agent_jobs")
for name in os.listdir(spool_dir):
    mode = stat.S_IMODE(os.stat(os.path.join(spool_dir, name)).st_mode)
    assert mode == 0o600, (name, oct(mode))

# Foreground exec uses the same literal IR. Metacharacters remain one argv
# item through a real two-stage pipeline.
literal = b"literal ; $(never) * $HOME\n"
pipeline = {
    "version": 1,
    "pipeline": [
        {"argv": [python, "-c", f"import os; os.write(1,{literal!r})"]},
        {"argv": ["/bin/cat"]},
    ],
}
execute_request = {
    "id": "pipeline", "protocol_version": 2, "op": "exec",
    "ir": pipeline, "caps": create_caps,
}
execute_wire = json.dumps(execute_request, separators=(",", ":"))
executed = request(proc, execute_request, execute_wire)
assert executed["state"] == "done" and executed["rc"] == 0, executed
assert request(proc, execute_request, execute_wire) == executed
pipeline_out = job_read(proc, "pipeline-read", executed["job_id"], "stdout", 0)
assert base64.b64decode(pipeline_out["data_b64"], validate=True) == literal

# IR redirections 0 and 1 are literal and override the default spools.
redir_in = os.path.join(root, "redir.in")
redir_out = os.path.join(root, "redir.out")
redir_err = os.path.join(root, "redir.err")
open(redir_in, "wb").write(b"REDIR\x00DATA")
redirected = request(proc, {
    "id": "redir", "protocol_version": 2, "op": "exec",
    "ir": {"version": 1, "pipeline": [{
        "argv": [python, "-c", "import os; d=os.read(0,99); os.write(1,d); os.write(2,b'REDIR_ERR')"],
        "redirs": [
            {"fd": 0, "kind": "read", "path": redir_in},
            {"fd": 1, "kind": "truncate", "path": redir_out},
            {"fd": 2, "kind": "append", "path": redir_err},
        ],
    }]},
    "caps": create_caps,
})
assert redirected["rc"] == 0 and open(redir_out, "rb").read() == b"REDIR\x00DATA"
assert open(redir_err, "rb").read() == b"REDIR_ERR"
empty = job_read(proc, "redir-read", redirected["job_id"], "stdout", 0)
assert base64.b64decode(empty["data_b64"], validate=True) == b"" and empty["eof"]

# TERM is sent once and escalated to KILL for a process that ignores it.
ignore = "import os,signal,time; signal.signal(signal.SIGTERM,signal.SIG_IGN); os.write(1,b'READY\\n'); time.sleep(30)"
cancel_start = request(proc, {
    "id": "cancel-start", "protocol_version": 2, "op": "job.start",
    "argv": [python, "-c", ignore], "background": True,
    "caps": create_caps,
})
cancel_id = cancel_start["job_id"]
read_until(proc, cancel_id, "stdout", b"READY")
nonpty_write = request(proc, {
    "id": "nonpty-write", "protocol_version": 2, "op": "job.write",
    "job_id": cancel_id, "data_b64": "eA==", "caps": ["job:pty:write"],
})
assert nonpty_write["error"] == "job has no PTY", nonpty_write
cancel_body = {
    "id": "cancel-one", "protocol_version": 2, "op": "job.cancel",
    "job_id": cancel_id, "grace_ms": 30, "caps": ["job:signal"],
}
cancel_wire = json.dumps(cancel_body, separators=(",", ":"))
cancelled, cancelled_raw = request_raw(proc, cancel_body, cancel_wire)
assert cancelled["cancel_applied"] is True and cancelled["state"] == "done", cancelled
assert cancelled["cancel_requested"] is True and cancelled["kill_sent"] is True, cancelled
assert cancelled["quiescent"] is True and cancelled["quiescence_forced"] is True, cancelled
cancelled_replay, cancelled_replay_raw = request_raw(proc, cancel_body, cancel_wire)
assert cancelled_replay == cancelled and cancelled_replay_raw == cancelled_raw
cancel_conflict = request(proc, {
    **cancel_body, "grace_ms": 31,
})
assert cancel_conflict["error"] == "idempotency conflict", cancel_conflict
cancelled_again = request(proc, {
    "id": "cancel-two", "protocol_version": 2, "op": "job.cancel",
    "job_id": cancel_id, "grace_ms": 30, "caps": ["job:signal"],
})
assert cancelled_again["cancel_applied"] is False, cancelled_again

# Linux PTY supplies a controlling terminal and supports live resize.
pty_code = """import os,signal,struct,termios,time
seen=False
def winch(*_):
 global seen; seen=True
signal.signal(signal.SIGWINCH,winch)
r,c=termios.tcgetwinsize(1)
os.write(1,f'PTY_READY tty={int(os.isatty(0) and os.isatty(1))} rows={r} cols={c}\\n'.encode())
data=os.read(0,1024)
os.write(1,b'PTY_INPUT='+data)
while not seen: time.sleep(.01)
r,c=termios.tcgetwinsize(1)
os.write(1,f'PTY_RESIZED rows={r} cols={c}\\n'.encode())
"""
if sys.platform.startswith("linux"):
    pty_start = request(proc, {
        "id": "pty-start", "protocol_version": 2, "op": "job.start",
        "argv": [python, "-c", pty_code], "background": True,
        "pty": True, "rows": 21, "cols": 61,
        "caps": create_caps + ["job:pty:create"],
    })
    pty_id = pty_start["job_id"]
    pty_data, pty_offset = read_until(proc, pty_id, "stdout", b"PTY_READY")
    assert b"tty=1 rows=21 cols=61" in pty_data, pty_data
    denied_write = request(proc, {
        "id": "pty-write-denied", "protocol_version": 2, "op": "job.write",
        "job_id": pty_id, "data_b64": "eA==", "caps": ["job:read"],
    })
    assert denied_write["error"] == "capability denied", denied_write
    oversized_write = request(proc, {
        "id": "pty-write-oversized", "protocol_version": 2, "op": "job.write",
        "job_id": pty_id,
        "data_b64": base64.b64encode(b"x" * 16385).decode(),
        "caps": ["job:pty:write"],
    })
    assert "error" in oversized_write, oversized_write
    malformed_write = request(proc, {
        "id": "pty-write-malformed", "protocol_version": 2, "op": "job.write",
        "job_id": pty_id, "data_b64": "AB==",
        "caps": ["job:pty:write"],
    })
    assert "error" in malformed_write, malformed_write
    pty_input = b"input\x00\xff\n"
    write_body = {
        "id": "pty-write", "protocol_version": 2, "op": "job.write",
        "job_id": pty_id,
        "data_b64": base64.b64encode(pty_input).decode(),
        "caps": ["job:pty:write"],
    }
    write_wire = json.dumps(write_body, separators=(",", ":"))
    written, written_raw = request_raw(proc, write_body, write_wire)
    assert written["accepted_bytes"] == len(pty_input), written
    assert written["state"] == "running" and written["receipt_phase"] == "action", written
    written_replay, written_replay_raw = request_raw(proc, write_body, write_wire)
    assert written_replay == written and written_replay_raw == written_raw
    write_conflict = request(proc, {**write_body, "data_b64": "eA=="})
    assert write_conflict["error"] == "idempotency conflict", write_conflict
    pty_data, pty_offset = read_until(
        proc, pty_id, "stdout", b"PTY_INPUT=" + pty_input.rstrip(b"\n")
    )
    resize_body = {
        "id": "pty-resize", "protocol_version": 2, "op": "job.resize",
        "job_id": pty_id, "rows": 33, "cols": 99,
        "caps": ["job:pty:resize"],
    }
    resize_wire = json.dumps(resize_body, separators=(",", ":"))
    resized, resized_raw = request_raw(proc, resize_body, resize_wire)
    assert resized["rows"] == 33 and resized["cols"] == 99, resized
    assert resized["receipt_phase"] == "action", resized
    resized_replay, resized_replay_raw = request_raw(proc, resize_body,
                                                     resize_wire)
    assert resized_replay == resized and resized_replay_raw == resized_raw
    resize_conflict = request(proc, {**resize_body, "cols": 100})
    assert resize_conflict["error"] == "idempotency conflict", resize_conflict
    pty_done = request(proc, {
        "id": "pty-wait", "protocol_version": 2, "op": "job.wait",
        "job_id": pty_id, "wait_ms": 2000, "caps": ["job:read"],
    })
    assert pty_done["rc"] == 0, pty_done
    assert pty_done["receipt_phase"] == "terminal", pty_done
    assert pty_done["start_receipt_ref"] == pty_start["receipt_ref"], pty_done
    pty_tail = job_read(proc, "pty-tail", pty_id, "stdout", pty_offset)
    pty_data += base64.b64decode(pty_tail["data_b64"], validate=True)
    assert b"PTY_RESIZED rows=33 cols=99" in pty_data, pty_data
    terminal_write = request(proc, {
        "id": "pty-write-terminal", "protocol_version": 2, "op": "job.write",
        "job_id": pty_id, "data_b64": "eA==", "caps": ["job:pty:write"],
    })
    assert terminal_write["error"] == "job is already terminal", terminal_write

    # A raw PTY child that never reads eventually applies backpressure. Every
    # write returns within its finite deadline, and timeout does not wedge the
    # daemon: read and cancel remain available afterwards.
    blocked_start = request(proc, {
        "id": "blocked-start", "protocol_version": 2, "op": "job.start",
        "argv": [python, "-c",
                 "import os,termios,time; x=termios.tcgetattr(0); "
                 "x[3]&=~(termios.ECHO|termios.ICANON); termios.tcsetattr(0,termios.TCSANOW,x); "
                 "os.write(1,b'BLOCKED_READY\\n'); time.sleep(30)"],
        "background": True, "pty": True,
        "caps": create_caps + ["job:pty:create"],
    })
    blocked_id = blocked_start["job_id"]
    read_until(proc, blocked_id, "stdout", b"BLOCKED_READY")
    blocked = None
    for attempt in range(16):
        body = {
            "id": f"blocked-write-{attempt}", "protocol_version": 2,
            "op": "job.write", "job_id": blocked_id, "wait_ms": 10,
            "data_b64": base64.b64encode(b"z" * 16384).decode(),
            "caps": ["job:pty:write"],
        }
        before = time.monotonic()
        row = request(proc, body)
        assert time.monotonic() - before < 0.5, row
        if row.get("outcome") == "indeterminate":
            blocked = row
            break
        assert row["accepted_bytes"] == 16384, row
    assert blocked is not None and 0 <= blocked["accepted_bytes"] < 16384, blocked
    blocked_retry = request(proc, body)
    assert blocked_retry["outcome"] == "indeterminate", blocked_retry
    assert blocked_retry["error"] == "request outcome unknown", blocked_retry
    blocked_read = job_read(proc, "blocked-read-after-timeout", blocked_id,
                            "stdout", 0)
    assert b"BLOCKED_READY" in base64.b64decode(blocked_read["data_b64"]), blocked_read
    blocked_cancel = request(proc, {
        "id": "blocked-cancel", "protocol_version": 2, "op": "job.cancel",
        "job_id": blocked_id, "grace_ms": 10, "caps": ["job:signal"],
    })
    assert blocked_cancel["state"] == "done", blocked_cancel

# Limits and authority fail before execution.
for bad in (
    {"id": "bad-version", "protocol_version": 2, "op": "exec",
     "ir": {"version": 2, "pipeline": [{"argv": ["/bin/true"]}]},
     "caps": create_caps},
    {"id": "bad-cap", "protocol_version": 2, "op": "job.start",
     "argv": [python, "-c", f"open({os.path.join(root,'denied')!r},'w').write('x')"],
     "background": True, "caps": ["effect:exec"]},
    {"id": "shell-text", "protocol_version": 2, "op": "exec",
     "line": "touch forbidden", "caps": create_caps},
    {"id": "not-background", "protocol_version": 2, "op": "job.start",
     "argv": ["/bin/true"], "background": False, "caps": create_caps},
    {"id": "too-many-stages", "protocol_version": 2, "op": "exec",
     "ir": {"version": 1, "pipeline": [
         {"argv": ["/bin/true"]} for _ in range(17)
     ]}, "caps": create_caps},
):
    assert "error" in request(proc, bad), bad
assert not os.path.exists(os.path.join(root, "denied"))
context_denied = os.path.join(root, "context-denied")
context_effect = [python, "-c", f"open({context_denied!r},'ab').write(b'x')"]
cwd_file = os.path.join(root, "not-a-directory")
open(cwd_file, "w").write("file")
context_rejections = (
    ({"id": "protected-home", "protocol_version": 2, "op": "exec",
      "argv": context_effect, "env": {"HOME": os.path.join(root, "moved")},
      "caps": create_caps}, "protected env"),
    ({"id": "protected-cancel-fd", "protocol_version": 2, "op": "exec",
      "argv": context_effect, "env": {"JASH_AGENT_CANCEL_FD": "99"},
      "caps": create_caps}, "protected env"),
    ({"id": "protected-pwd", "protocol_version": 2, "op": "exec",
      "argv": context_effect, "cwd": context_dir, "env": {"PWD": "/tmp"},
      "caps": create_caps}, "protected env"),
    ({"id": "missing-cwd", "protocol_version": 2, "op": "exec",
      "argv": context_effect, "cwd": os.path.join(root, "missing"),
      "caps": create_caps}, "cwd"),
    ({"id": "file-cwd", "protocol_version": 2, "op": "exec",
      "argv": context_effect, "cwd": cwd_file, "caps": create_caps}, "cwd"),
    ({"id": "bad-job-stdin", "protocol_version": 2, "op": "exec",
      "argv": context_effect, "stdin_b64": "AB==", "caps": create_caps},
     "stdin_b64"),
    ({"id": "large-job-stdin", "protocol_version": 2, "op": "exec",
      "argv": context_effect,
      "stdin_b64": base64.b64encode(b"x" * 16385).decode(),
      "caps": create_caps}, "stdin_b64"),
    ({"id": "many-job-env", "protocol_version": 2, "op": "exec",
      "argv": context_effect, "env": {f"E{i}": "x" for i in range(17)},
      "caps": create_caps}, "env"),
    ({"id": "large-job-env", "protocol_version": 2, "op": "exec",
      "argv": context_effect, "env": {f"L{i}": "x" * 1900 for i in range(9)},
      "caps": create_caps}, "env"),
    ({"id": "large-job-env-value", "protocol_version": 2, "op": "exec",
      "argv": context_effect, "env": {"LARGE": "x" * 2048},
      "caps": create_caps}, "env value"),
    ({"id": "invalid-job-env-key", "protocol_version": 2, "op": "exec",
      "argv": context_effect, "env": {"BAD=NAME": "x"},
      "caps": create_caps}, "env key"),
    ({"id": "pty-with-stdin", "protocol_version": 2, "op": "job.start",
      "argv": context_effect, "background": True, "pty": True,
      "stdin_b64": "eA==",
      "caps": create_caps + ["job:pty:create"]}, "stdin_b64"),
)
for rejected, expected_error in context_rejections:
    rejection = request(proc, rejected)
    assert expected_error in rejection["error"], (rejected, rejection)
assert not os.path.exists(context_denied)
assert "error" in request(proc, {
    "id": "wait-limit", "protocol_version": 2, "op": "job.wait",
    "job_id": job_id, "wait_ms": 30001, "caps": ["job:read"],
})
assert "error" in request(proc, {
    "id": "read-limit", "protocol_version": 2, "op": "job.read",
    "job_id": job_id, "stream": "stdout", "offset": 0,
    "limit": 65537, "caps": ["job:read"],
})
assert "error" in request(proc, {
    "id": "grace-limit", "protocol_version": 2, "op": "job.cancel",
    "job_id": job_id, "grace_ms": 5001, "caps": ["job:signal"],
})
finish(proc)

# The table is bounded to 32 retained jobs. The 33rd request is effect-free.
proc, _, _ = daemon("capacity")
for index in range(32):
    row = request(proc, {
        "id": f"slot-{index}", "protocol_version": 2, "op": "job.start",
        "argv": ["/bin/true"], "background": True, "caps": create_caps,
    })
    assert row["accepted"] is True, (index, row)
overflow_marker = os.path.join(root, "overflow-marker")
full = request(proc, {
    "id": "slot-overflow", "protocol_version": 2, "op": "job.start",
    "argv": [python, "-c", f"open({overflow_marker!r},'w').write('x')"],
    "background": True, "caps": create_caps,
})
assert full["error"] == "job table full" and not os.path.exists(overflow_marker), full
finish(proc)

# EOF owns cleanup: a live worker is cancelled/reaped and private spools vanish.
proc, _, eof_home = daemon("eof")
pid_file = os.path.join(root, "eof-child.pid")
eof_job = request(proc, {
    "id": "eof-job", "protocol_version": 2, "op": "job.start",
    "argv": [python, "-c", f"import os,time; open({pid_file!r},'w').write(str(os.getpid())); time.sleep(30)"],
    "background": True, "caps": create_caps,
})
deadline = time.monotonic() + 2
child_pid_text = ""
while time.monotonic() < deadline:
    if os.path.exists(pid_file):
        child_pid_text = open(pid_file).read().strip()
        if child_pid_text.isdigit():
            break
    time.sleep(.01)
assert child_pid_text.isdigit(), child_pid_text
child_pid = int(child_pid_text)
finish(proc)
try:
    os.kill(child_pid, 0)
except ProcessLookupError:
    pass
else:
    raise AssertionError(f"EOF left child {child_pid} alive")
jobs_dir = os.path.join(eof_home, ".jash", "agent_jobs")
assert not os.path.exists(jobs_dir) or not os.listdir(jobs_dir), os.listdir(jobs_dir)
PY

echo "agent jobs protocol: PASS"
