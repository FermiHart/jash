#!/usr/bin/env python3
"""Experimental single-file autonomy, using agentd, bubblewrap and libjef.

Receipts authenticate opaque harness observations, NOT independent filesystem
measurements. Trust the harness, selected daemon/verifier, expected signing key,
host kernel and installed system tools. Tracked files may themselves be secrets:
the caller must approve these inputs. Untracked files and .git are never staged.
No live-tree apply, commands on handoff, replacement publication, or new executor.
Publication is a read-only artifact directory (not protection from its owner).
There is no concurrent-writer guarantee for the source; it is never written.
Interrupted candidate runs retain incomplete evidence and cannot verify. Linux
artifact publication has an explicit recovery journal; no live-tree recovery.
"""
import argparse
import ctypes
import errno
import fcntl
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import select
import shutil
import stat
import subprocess
import sys
import tempfile
import time

if __package__:
    from .autonomy_sandbox import build_argv
else:
    from autonomy_sandbox import build_argv

SCHEMA = "jash.autonomy.v1"
SCHEMA_RESOURCES = "jash.autonomy.v2"
FILE_MAX = 1024 * 1024
TOTAL_MAX = 8 * FILE_MAX
COUNT_MAX = 128
STREAM_MAX = 64 * FILE_MAX  # Existing supervisor's fixed output bound.
POLICY = {"sandbox": "autonomy_sandbox/v1", "inputs": "tracked-only-no-git",
          "mutation": "target-content-only", "selection": "bytes,sha256,id",
          "files": COUNT_MAX, "file_bytes": FILE_MAX, "total_bytes": TOTAL_MAX}
POLICY_RESOURCES = {**POLICY, "resources": "cgroup-v2"}
NOT_VERIFIED = ["independent filesystem measurement (trusted harness observations)",
                "semantic correctness beyond the frozen checker",
                "system executable identity and host kernel integrity",
                "intermediate nonterminal protocol exchanges (not signed by agentd)",
                "concurrent live-source writers", "live-tree application"]
ENV = {"PATH": "/usr/bin:/bin", "LANG": "C"}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def wire(value):
    return (json.dumps(value, sort_keys=True, separators=(",", ":"),
                       ensure_ascii=True, allow_nan=False) + "\n").encode()


def decode(raw):
    def pairs(items):
        result = {}
        for key, value in items:
            require(key not in result, "duplicate JSON key")
            result[key] = value
        return result
    return json.loads(raw.decode("utf-8"), object_pairs_hook=pairs,
                      parse_constant=lambda _: require(False, "non-finite JSON"))


def fields(obj, names):
    require(type(obj) is dict and set(obj) == set(names.split()), "invalid schema fields")


def relative(name):
    require(type(name) is str and 0 < len(name.encode()) <= 240 and
            "\x00" not in name and "\\" not in name, "invalid relative path")
    parts = name.split("/")
    require(all(p not in ("", ".", "..", ".git") for p in parts) and
            not PurePosixPath(name).is_absolute(), "unsafe relative path")
    return parts


def absolute(name):
    require(type(name) is str and name.startswith("/") and len(name.encode()) <= 512,
            "invalid absolute context path")
    relative(name[1:])
    return Path(name)


def canonical(path):
    path = Path(os.path.abspath(path))
    require(path.resolve(strict=True) == path and path.is_dir(),
            "directory must be canonical, without symlinks")
    return path


def new_path(path):
    path = Path(os.path.abspath(path))
    canonical(path.parent)
    require(not os.path.lexists(path), "destination already exists")
    relative(path.name)
    return path


def disjoint(a, b):
    require(a != b and a not in b.parents and b not in a.parents,
            "paths must be disjoint (source is never an output)")


def read_file(root, name, limit=FILE_MAX):
    """Open every component no-follow; reject special files before reading."""
    parts = relative(name)
    fd = os.open(root, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    try:
        for part in parts[:-1]:
            child = os.open(part, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=fd)
            os.close(fd)
            fd = child
        child = os.open(parts[-1], os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=fd)
        try:
            before = os.fstat(child)
            require(stat.S_ISREG(before.st_mode) and before.st_mode & 0o444 and
                    before.st_size <= limit, "not a readable bounded regular file: " + name)
            data = bytearray()
            while len(data) <= limit:
                chunk = os.read(child, min(65536, limit + 1 - len(data)))
                if not chunk:
                    break
                data.extend(chunk)
            after = os.fstat(child)
            signature = lambda s: (s.st_dev, s.st_ino, s.st_mode, s.st_size,
                                   s.st_mtime_ns, s.st_ctime_ns, s.st_nlink)
            require(len(data) <= limit and signature(before) == signature(after) and
                    signature(after) == signature(os.stat(parts[-1], dir_fd=fd,
                                                          follow_symlinks=False)),
                    "file changed while reading: " + name)
            return bytes(data), stat.S_IMODE(after.st_mode)
        finally:
            os.close(child)
    finally:
        os.close(fd)


def put(path, data, mode=0o600):
    with path.open("xb") as out:
        os.chmod(path, mode)
        out.write(data)
        out.flush()
        os.fsync(out.fileno())


def tracked(repo):
    git = ["/usr/bin/git", "--no-optional-locks", "-c", "core.fsmonitor=false",
           "-c", "core.untrackedCache=false", "-C", str(repo)]
    context = subprocess.run([*git, "rev-parse", "--show-prefix"],
                             env={**ENV, "HOME": "/nonexistent"}, capture_output=True, timeout=10)
    require(context.returncode == 0 and context.stdout == b"\n", "repo must be a Git worktree root")
    with tempfile.TemporaryFile(dir="/tmp") as out:
        result = subprocess.run([*git,
                                 "ls-files", "--cached", "--stage", "-z"],
                                env={**ENV, "HOME": "/nonexistent"}, stdout=out,
                                stderr=subprocess.DEVNULL, timeout=10)
        require(result.returncode == 0, "git tracked inventory failed")
        out.seek(0)
        raw = out.read(FILE_MAX + 1)
    require(len(raw) <= FILE_MAX and raw.endswith(b"\0"), "empty/oversized tracked inventory")
    rows = raw[:-1].split(b"\0")
    require(len(rows) <= COUNT_MAX, "too many tracked files")
    names = []
    for row in rows:
        meta, name = row.split(b"\t", 1)
        mode, _, stage = meta.split(b" ")
        require(mode in (b"100644", b"100755") and stage == b"0",
                "reject tracked symlink, gitlink or unmerged entry")
        name = name.decode("utf-8")
        relative(name)
        require(name not in names, "duplicate tracked path")
        names.append(name)
    return sorted(names)


def snapshot(repo):
    names = tracked(repo)
    before = {name: (repo / name).lstat() for name in names}
    data, manifest, total = {}, [], 0
    for name in names:
        content, mode = read_file(repo, name)
        require(mode <= 0o777, "special tracked mode bits are not supported")
        total += len(content)
        require(total <= TOTAL_MAX, "snapshot byte limit")
        data[name] = content
        manifest.append({"path": name, "sha256": sha(content), "mode": mode,
                         "bytes": len(content)})
    require(tracked(repo) == names, "index changed during snapshot")
    for entry in manifest:
        content, mode = read_file(repo, entry["path"])
        require(sha(content) == entry["sha256"] and mode == entry["mode"],
                "source changed during snapshot")
        first, last = before[entry["path"]], (repo / entry["path"]).lstat()
        require((first.st_ino, first.st_dev, first.st_mtime_ns, first.st_ctime_ns) ==
                (last.st_ino, last.st_dev, last.st_mtime_ns, last.st_ctime_ns),
                "source metadata changed during snapshot")
    return manifest, data


def reconstruct(work, manifest, contents):
    work.mkdir(mode=0o700)
    for entry in manifest:
        path = work / entry["path"]
        path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
        put(path, contents[entry["path"]], entry["mode"])
    return {e["path"]: (work / e["path"]).stat() for e in manifest}


def inspect_work(work, manifest, target, before):
    require(stat.S_IMODE(work.lstat().st_mode) == 0o700, "generator changed workspace mode")
    expected = {e["path"]: e for e in manifest}
    directories = {str(p) for name in expected for p in PurePosixPath(name).parents
                   if str(p) != "."}
    seen = set()
    for parent, dirs, files in os.walk(work, followlinks=False):
        for name in dirs + files:
            path = Path(parent) / name
            rel = path.relative_to(work).as_posix()
            info = path.lstat()
            if stat.S_ISDIR(info.st_mode):
                require(rel in directories and stat.S_IMODE(info.st_mode) == 0o700,
                        "generator created or changed a directory")
            else:
                require(rel in expected and stat.S_ISREG(info.st_mode) and info.st_nlink == 1,
                        "generator changed target type or created a file/link")
                seen.add(rel)
    require(seen == set(expected), "generator removed a tracked file")
    candidate = None
    for name, entry in expected.items():
        data, mode = read_file(work, name)
        require(mode == entry["mode"], "generator changed file mode")
        require(name == target or sha(data) == entry["sha256"],
                "generator modified a non-target file")
        if name != target:
            first, last = before[name], (work / name).lstat()
            require((first.st_ino, first.st_mtime_ns, first.st_ctime_ns) ==
                    (last.st_ino, last.st_mtime_ns, last.st_ctime_ns),
                    "generator replaced or touched a non-target file")
        if name == target:
            candidate = data
    return candidate


def command(value):
    require(type(value) is list and 1 <= len(value) <= 32 and
            all(type(v) is str and "\0" not in v and len(v.encode()) <= 4096 for v in value)
            and 0 < sum(len(v.encode()) + 1 for v in value) <= 8192 and
            value[0] and not value[0].startswith("-"), "invalid bounded JSON argv")
    return value


def validate_resources(resources):
    # The cgroup-v2 backend fixes memory.swap.max=0 and memory.oom.group=1;
    # neither is caller-selectable. The five limits are receipt-bound below.
    fields(resources, "memory_max_bytes pids_max cpu_quota_us cpu_period_us wall_timeout_ms")
    require(all(type(value) is int for value in resources.values()),
            "resource limits must be integers, not booleans")
    require(16 * 1024 * 1024 <= resources["memory_max_bytes"] <= 1024 ** 4 and
            1 <= resources["pids_max"] <= 4096 and
            1000 <= resources["cpu_period_us"] <= 1000000 and
            1000 <= resources["cpu_quota_us"] <= resources["cpu_period_us"] and
            1 <= resources["wall_timeout_ms"] <= 30000, "resource limit bounds")


def cgroup_root(path):
    name = os.fspath(path)
    root = absolute(name)
    require(str(root) == name and canonical(root) == root,
            "cgroup root must be a canonical absolute directory")
    # No Python cgroup probes or executor: agentd pins the root at startup and
    # validates enforcement for each resource request before execution.
    return root


def validate_resource_response(row, resources, terminal):
    validate_resources(row.get("resources"))
    require(row["resources"] == resources and row.get("resource_backend") == "cgroup-v2"
            and row.get("resource_enforced") is True, "resource enforcement mismatch")
    if terminal:
        require(all(type(row.get(k)) is bool for k in
                    ("resource_timed_out", "resource_violation", "resource_empty", "descendants_forced")) and
                all(type(row.get(k)) is int and row[k] >= 0 for k in
                    ("resource_memory_events", "resource_pids_events")),
                "invalid terminal resource observations")


def validate_contract(c):
    require(type(c) is dict, "invalid schema fields")
    resource_mode = c.get("schema") == SCHEMA_RESOURCES
    fields(c, "schema campaign_id repo target baseline generators check policy timeout_ms" +
           (" resources" if resource_mode else ""))
    require(c["schema"] in (SCHEMA, SCHEMA_RESOURCES) and
            (wire(c["policy"]) == wire(POLICY_RESOURCES) if resource_mode else c["policy"] == POLICY),
            "unknown campaign policy")
    if resource_mode:
        validate_resources(c["resources"])
    require(type(c["campaign_id"]) is str and re.fullmatch("[0-9a-f]{32}", c["campaign_id"]),
            "invalid campaign identity")
    source = absolute(c["repo"])
    require(source != Path("/tmp"), "repo cannot contain the private temporary storage root")
    for exposed in ("/usr", "/bin", "/lib", "/lib64"):
        disjoint(source, Path(exposed))
    relative(c["target"])
    require(type(c["timeout_ms"]) is int and 1 <= c["timeout_ms"] <= 30000, "timeout bound")
    command(c["check"])
    require(type(c["generators"]) is list and 1 <= len(c["generators"]) <= 2,
            "one or two planned candidates required")
    for i, generator in enumerate(c["generators"]):
        fields(generator, "id argv")
        require(generator["id"] == f"c{i}", "invalid planned candidate ID")
        command(generator["argv"])
    require(type(c["baseline"]) is list and 1 <= len(c["baseline"]) <= COUNT_MAX,
            "invalid baseline")
    names, total = [], 0
    for entry in c["baseline"]:
        fields(entry, "path sha256 mode bytes")
        relative(entry["path"])
        require(type(entry["sha256"]) is str and re.fullmatch("[0-9a-f]{64}", entry["sha256"])
                and type(entry["mode"]) is int and 0 <= entry["mode"] <= 0o777 and
                entry["mode"] & 0o444 and
                type(entry["bytes"]) is int and 0 <= entry["bytes"] <= FILE_MAX,
                "invalid baseline entry")
        names.append(entry["path"])
        total += entry["bytes"]
    require(names == sorted(set(names)) and c["target"] in names and total <= TOTAL_MAX,
            "invalid baseline paths or bounds")


def validate_sandbox(argv, workspace, payload, writable):
    """Portable validation of the fixed builder policy, without host probes."""
    work = absolute(workspace)
    for exposed in ("/usr", "/bin", "/lib", "/lib64"):
        disjoint(work, Path(exposed))
    require(type(argv) is list and all(type(a) is str for a in argv), "invalid sandbox argv")
    prefix = ["/usr/bin/bwrap", "--unshare-all", "--unshare-user", "--unshare-cgroup",
              "--disable-userns", "--assert-userns-disabled", "--die-with-parent",
              "--new-session", "--as-pid-1", "--cap-drop", "ALL", "--clearenv",
              "--setenv", "PATH", "/usr/bin:/bin", "--setenv", "HOME", "/tmp",
              "--setenv", "LANG", "C"]
    require(argv[:len(prefix)] == prefix, "sandbox policy prefix mismatch")
    pos = len(prefix)
    for dest in ("/usr", "/bin", "/lib", "/lib64"):
        triple = argv[pos:pos + 3]
        if dest != "/usr" and (len(triple) != 3 or triple[2] != dest):
            continue
        require(triple == ["--ro-bind", dest, dest] or
                (dest != "/usr" and triple == ["--symlink", "/usr" + dest, dest]),
                "unsupported system mount layout")
        pos += 3
    require(argv[pos:] == ["--proc", "/proc", "--dev", "/dev", "--tmpfs", "/tmp",
                           "--bind" if writable else "--ro-bind", workspace, "/work",
                           "--chdir", "/work", "--", *payload], "sandbox policy tail mismatch")


class Daemon:
    def __init__(self, binary, home, trusted_cgroup_root=None):
        self.home, self.pending = home, b""
        env = {**ENV, "HOME": str(home)}
        if trusted_cgroup_root is not None:
            env["JASH_AGENT_CGROUP_ROOT"] = str(cgroup_root(trusted_cgroup_root))
        self.proc = subprocess.Popen([str(Path(binary).resolve(strict=True))], cwd=home,
                                     env=env, stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                     bufsize=0, close_fds=True)
        try:
            hello = decode(self.line(5))
            self.key = hello["receipts"]["pubkey"]
            require(hello["runtime"] == "jash-agentd" and
                    re.fullmatch("[0-9a-f]{64}", self.key), "invalid daemon handshake")
        except Exception:
            self.close()
            raise

    def line(self, timeout):
        deadline = time.monotonic() + timeout
        while b"\n" not in self.pending:
            remaining = deadline - time.monotonic()
            require(remaining > 0 and select.select([self.proc.stdout], [], [], remaining)[0],
                    "daemon response deadline exceeded")
            chunk = os.read(self.proc.stdout.fileno(), 65536)
            require(chunk and len(self.pending) + len(chunk) <= FILE_MAX, "bad daemon framing")
            self.pending += chunk
        line, self.pending = self.pending.split(b"\n", 1)
        return line + b"\n"

    def exchange(self, directory, label, body, timeout=5):
        request = wire(body)
        require(len(request) <= 16384, "request too large")
        put(directory / (label + "-request.json"), request)
        # A bounded nonblocking write prevents a wedged daemon blocking forever.
        fd = self.proc.stdin.fileno()
        os.set_blocking(fd, False)
        deadline, offset = time.monotonic() + 5, 0
        while offset < len(request):
            remaining = deadline - time.monotonic()
            require(remaining > 0 and select.select([], [fd], [], remaining)[1],
                    "daemon request deadline exceeded")
            try:
                offset += os.write(fd, request[offset:])
            except BlockingIOError:
                pass
        raw = self.line(timeout)
        put(directory / (label + "-response.json"), raw)
        row = decode(raw)
        require(row.get("id") == body["id"] and "error" not in row,
                "daemon rejected request: " + str(row))
        if "receipt_ref" in row:
            ref = row["receipt_ref"]
            require(type(ref) is str and re.fullmatch(r"[0-9a-f.\-]+job-(start|terminal)\.dsse", ref),
                    "invalid receipt reference")
            put(directory / "agent_receipts" / ref,
                read_file(self.home / ".jash/agent_receipts", ref)[0])
        if row.get("terminal") is True:
            for stream in ("stdout", "stderr"):
                ref = row[stream + "_ref"]
                require(type(ref) is str and re.fullmatch(r"blake3-[0-9a-f]{64}-[0-9]+\.bin", ref),
                        "invalid stream reference")
                path = directory / "overflow" / ref
                content = read_file(self.home / ".jash/overflow", ref, STREAM_MAX)[0]
                if path.exists():
                    require(path.read_bytes() == content, "conflicting stream")
                else:
                    put(path, content)
        return row

    def job(self, directory, spec):
        validate_contract(spec["campaign"])
        require(spec["schema"] == spec["campaign"]["schema"], "phase identity mismatch")
        resources = spec["campaign"].get("resources")
        directory.mkdir(mode=0o700)
        (directory / "agent_receipts").mkdir()
        (directory / "overflow").mkdir()
        raw = wire(spec)
        require(len(raw) <= FILE_MAX, "spec too large")
        put(directory / "spec.json", raw)
        ident = spec["candidate"] + "-" + spec["phase"]
        start = self.exchange(directory, "start", {
            "id": ident + "-start", "protocol_version": 2, "op": "job.start",
            "argv": spec["argv"], "background": True,
            "execution_spec_sha256": "sha256:" + sha(raw),
            "caps": ["effect:exec", "job:create"],
            **({"resources": resources} if resources is not None else {})})
        if resources is not None:
            validate_resource_response(start, resources, False)
        wait = {"id": ident + "-wait", "protocol_version": 2, "op": "job.wait",
                "job_id": start["job_id"], "wait_ms": spec["campaign"]["timeout_ms"],
                "caps": ["job:read"]}
        row = self.exchange(directory, "wait", wait, wait["wait_ms"] / 1000 + 5)
        if row.get("terminal") is True:
            for kind in ("request", "response"):
                (directory / ("wait-" + kind + ".json")).rename(
                    directory / ("terminal-" + kind + ".json"))
        else:
            self.exchange(directory, "cancel", {
                "id": ident + "-cancel", "protocol_version": 2, "op": "job.cancel",
                "job_id": start["job_id"], "grace_ms": 0, "caps": ["job:signal"]})
            wait.update(id=ident + "-timeout", wait_ms=5000)
            row = self.exchange(directory, "terminal", wait, 10)
        require(row.get("terminal") is True and row.get("quiescent") is True,
                "job failed to reach a quiescent terminal state; evidence retained")
        if resources is not None:
            validate_resource_response(row, resources, True)
        return row

    def close(self):
        self.proc.stdin.close()
        try:
            self.proc.wait(timeout=8)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait(timeout=3)
        self.proc.stdout.close()


def job_passed(row, request, ident, resources=None):
    if resources is not None:
        try:
            validate_resource_response(row, resources, True)
        except ValueError:
            return False
        if (row["resource_timed_out"] or row["resource_violation"] or not row["resource_empty"]
                or row["descendants_forced"]
                or row["resource_memory_events"] or row["resource_pids_events"]):
            return False
    return (row.get("state") == "done" and type(row.get("rc")) is int and row["rc"] == 0
            and type(row.get("signal")) is int and row["signal"] == 0
            and row.get("terminal") is True and row.get("quiescent") is True
            and all(row.get(k) is False for k in
                    ("cancel_requested", "output_limit", "quiescence_forced", "kill_sent"))
            and request["id"] == ident + "-wait")


def verify_phase(directory, key, verifier, campaign, cid, phase):
    spec = decode(read_file(directory, "spec.json")[0])
    fields(spec, "schema campaign candidate phase workspace argv" +
           (" artifact_sha256 artifact_bytes artifact_source generation_valid baseline_current generation_receipt_sha256"
            if phase == "check" else ""))
    validate_contract(spec["campaign"])
    require(spec["schema"] == spec["campaign"]["schema"] and spec["candidate"] == cid and
            spec["phase"] == phase, "phase identity mismatch")
    if campaign is not None:
        require(wire(spec["campaign"]) == wire(campaign), "campaign mismatch")
    campaign = spec["campaign"]
    resources = campaign.get("resources")
    disjoint(absolute(spec["workspace"]), absolute(campaign["repo"]))
    payload = (campaign["check"] if phase == "check" else
               campaign["generators"][int(cid[1])]["argv"])
    validate_sandbox(spec["argv"], spec["workspace"], payload, phase == "generate")
    requests, responses, receipts = [], [], []
    ident = cid + "-" + phase
    for label in ("start", "terminal"):
        req = decode(read_file(directory, label + "-request.json")[0])
        res = decode(read_file(directory, label + "-response.json")[0])
        ref = res["receipt_ref"]
        require(type(ref) is str and re.fullmatch(r"[0-9a-f.\-]+job-(start|terminal)\.dsse", ref),
                "invalid receipt ref")
        read_file(directory, "agent_receipts/" + ref)
        requests.append(req)
        responses.append(res)
        receipts.append(directory / "agent_receipts" / ref)
    fields(requests[0], "id protocol_version op argv background execution_spec_sha256 caps" +
           (" resources" if resources is not None else ""))
    if resources is not None:
        validate_resources(requests[0]["resources"])
        for i, res in enumerate(responses):
            validate_resource_response(res, resources, i == 1)
    require(requests[0] == {"id": ident + "-start", "protocol_version": 2,
            "op": "job.start", "argv": spec["argv"], "background": True,
            "execution_spec_sha256": "sha256:" + sha(read_file(directory, "spec.json")[0]),
            "caps": ["effect:exec", "job:create"],
            **({"resources": resources} if resources is not None else {})}, "start/spec mismatch")
    fields(requests[1], "id protocol_version op job_id wait_ms caps")
    require(requests[1] == {"id": requests[1]["id"], "protocol_version": 2, "op": "job.wait",
            "job_id": responses[0]["job_id"], "wait_ms": campaign["timeout_ms"]
            if requests[1]["id"] == ident + "-wait" else 5000, "caps": ["job:read"]}
            and requests[1]["id"] in (ident + "-wait", ident + "-timeout"),
            "terminal request mismatch")
    expected = {"spec.json", "agent_receipts", "overflow", "start-request.json",
                "start-response.json", "terminal-request.json", "terminal-response.json"}
    if requests[1]["id"] == ident + "-timeout":
        for label, op, caps, limits in (("wait", "job.wait", ["job:read"],
                                         {"wait_ms": campaign["timeout_ms"]}),
                                        ("cancel", "job.cancel", ["job:signal"], {"grace_ms": 0})):
            expected.update((label + "-request.json", label + "-response.json"))
            req = decode(read_file(directory, label + "-request.json")[0])
            res = decode(read_file(directory, label + "-response.json")[0])
            require(req == {"id": ident + "-" + label, "protocol_version": 2, "op": op,
                            "caps": caps, "job_id": responses[0]["job_id"], **limits}
                    and res.get("id") == req["id"], "intermediate protocol mismatch")
            if resources is not None:
                validate_resource_response(res, resources, res.get("terminal") is True)
            if "receipt_ref" in res:
                ref = res["receipt_ref"]
                require(type(ref) is str and re.fullmatch(r"[0-9a-f.\-]+job-terminal\.dsse", ref),
                        "invalid intermediate receipt")
                receipt = directory / "agent_receipts" / ref
                verified = subprocess.run([str(verifier), "job-receipt", key, "terminal",
                    str(directory / (label + "-request.json")),
                    str(directory / (label + "-response.json")), str(receipt)],
                    env={**ENV, "HOME": "/nonexistent"}, capture_output=True, timeout=10)
                require(verified.returncode == 0, "invalid intermediate terminal receipt")
                receipts.append(receipt)
                responses.append(res)
    require(set(p.name for p in directory.iterdir()) == expected, "unexpected phase metadata")
    require(set(p.name for p in (directory / "agent_receipts").iterdir()) ==
            {p.name for p in receipts}, "unreferenced receipt")
    require(set(p.name for p in (directory / "overflow").iterdir()) ==
            {r[s + "_ref"] for r in responses if r.get("terminal") is True
             for s in ("stdout", "stderr")}, "missing/unreferenced stream")
    result = subprocess.run([str(verifier), "job-spec", key, str(directory / "spec.json"),
                             str(directory / "start-request.json"),
                             str(directory / "start-response.json"), str(receipts[0]),
                             str(directory / "terminal-request.json"),
                             str(directory / "terminal-response.json"), str(receipts[1])],
                            env={**ENV, "HOME": "/nonexistent"}, capture_output=True, timeout=10)
    require(result.returncode == 0, "libjef job-spec verification failed: " + cid + "/" + phase)
    return spec, job_passed(responses[1], requests[1], ident, resources), sha(receipts[1].read_bytes()), responses[1]["rc"]


def verify_bundle(bundle, key, verifier):
    require(type(key) is str and re.fullmatch("[0-9a-fA-F]{64}", key), "external pubkey required")
    bundle = canonical(bundle)
    # libjef opens multiple paths. Verify a bounded private copy, never mutable
    # attacker-controlled paths, and return these exact verified artifact bytes.
    with tempfile.TemporaryDirectory(prefix="autonomy-verify-", dir="/tmp") as temp:
        root = Path(temp)
        count, total = 0, 0
        for parent, dirs, files in os.walk(bundle, followlinks=False):
            for name in dirs + files:
                path = Path(parent) / name
                rel = path.relative_to(bundle).as_posix()
                relative(rel)
                info = path.lstat()
                count += 1
                require(count <= 200 and not stat.S_ISLNK(info.st_mode), "invalid bundle tree")
                if stat.S_ISDIR(info.st_mode):
                    (root / rel).mkdir(mode=0o700)
                else:
                    content = read_file(bundle, rel, STREAM_MAX if "/overflow/" in rel else FILE_MAX)[0]
                    total += len(content)
                    require(total <= 8 * STREAM_MAX + TOTAL_MAX, "bundle byte limit")
                    put(root / rel, content)
        index = decode(read_file(root, "index.json")[0])
        fields(index, "schema candidates")
        require(index["schema"] in (SCHEMA, SCHEMA_RESOURCES) and
                index["candidates"] in (["c0"], ["c0", "c1"]),
                "invalid bundle index")
        campaign, evidence, artifacts = None, [], {}
        require(set(p.name for p in root.iterdir()) == {"index.json", *index["candidates"]},
                "unexpected bundle entries")
        for cid in index["candidates"]:
            directory = root / cid
            require(set(p.name for p in directory.iterdir()) == {"generate", "check", "artifact"},
                    "invalid candidate layout")
            gen, gen_ok, gen_receipt, gen_rc = verify_phase(directory / "generate", key, verifier,
                                                         campaign, cid, "generate")
            campaign = gen["campaign"]
            require(index["schema"] == campaign["schema"], "bundle/campaign schema mismatch")
            require(index["candidates"] == [g["id"] for g in campaign["generators"]],
                    "planned candidate omission/substitution")
            check, check_ok, check_receipt, check_rc = verify_phase(
                directory / "check", key, verifier, campaign, cid, "check")
            artifact = read_file(directory, "artifact")[0]
            require(type(check["artifact_bytes"]) is int and check["artifact_bytes"] == len(artifact)
                    and check["artifact_sha256"] == sha(artifact)
                    and check["generation_receipt_sha256"] == gen_receipt
                    and type(check["generation_valid"]) is bool
                    and check["artifact_source"] == ("generated-target" if check["generation_valid"]
                                                     else "baseline-fallback-ineligible")
                    and type(check["baseline_current"]) is bool, "artifact/observation mismatch")
            eligible = gen_ok and check_ok and check["generation_valid"] and check["baseline_current"]
            artifacts[cid] = artifact
            evidence.append({"id": cid, "eligible": eligible, "generation_rc": gen_rc,
                             "check_rc": check_rc, "generation_valid": check["generation_valid"],
                             "artifact_source": check["artifact_source"],
                             "baseline_current": check["baseline_current"],
                             "sha256": sha(artifact), "bytes": len(artifact),
                             "generation_receipt_sha256": gen_receipt,
                             "check_receipt_sha256": check_receipt})
        eligible = [e for e in evidence if e["eligible"]]
        winner = min(eligible, key=lambda e: (e["bytes"], e["sha256"], e["id"]))["id"] if eligible else None
        result = {"schema": campaign["schema"], "verified": True, "winner": winner,
                  "evidence": evidence, "campaign": campaign, "not_verified": NOT_VERIFIED}
        return result, artifacts.get(winner)


def current(repo, campaign):
    try:
        return (str(canonical(repo)) == campaign["repo"] and
                snapshot(canonical(repo))[0] == campaign["baseline"])
    except (OSError, ValueError, subprocess.SubprocessError):
        return False


def run(args):
    # Quotas require explicit host delegation. No opt-in means shipped v1 bytes
    # and policy, with no quota claims; inherited cgroup settings are never used.
    root, resources = getattr(args, "cgroup_root", None), getattr(args, "resources", None)
    require(resources is None or root is not None, "--resources requires --cgroup-root")
    if root is not None:
        root = cgroup_root(root)
        resources = (decode(resources.encode()) if resources is not None else
                     {"memory_max_bytes": 128 * 1024 * 1024, "pids_max": 64,
                      "cpu_quota_us": 50000, "cpu_period_us": 100000,
                      "wall_timeout_ms": args.timeout_ms})
        validate_resources(resources)
    repo, bundle, home = canonical(args.repo), new_path(args.bundle), new_path(args.runtime_home)
    require(repo != Path("/tmp"), "repo cannot contain the private temporary storage root")
    for a, b in ((repo, bundle), (repo, home), (bundle, home)):
        disjoint(a, b)
    for path in (repo, bundle, home):
        for exposed in ("/usr", "/bin", "/lib", "/lib64"):
            disjoint(path, Path(exposed).resolve())
    baseline, contents = snapshot(repo)
    campaign = {"schema": SCHEMA_RESOURCES if root is not None else SCHEMA,
                "campaign_id": os.urandom(16).hex(),
                "repo": str(repo), "target": args.target, "baseline": baseline,
                "generators": [{"id": f"c{i}", "argv": command(decode(g.encode()))}
                               for i, g in enumerate(args.generate)],
                "check": command(decode(args.check.encode())),
                "policy": POLICY_RESOURCES if root is not None else POLICY,
                "timeout_ms": args.timeout_ms,
                **({"resources": resources} if root is not None else {})}
    validate_contract(campaign)
    home.mkdir(mode=0o700)
    bundle.mkdir(mode=0o700)
    daemon = None
    try:
        daemon = Daemon(args.agentd, home, trusted_cgroup_root=root)
        for generator in campaign["generators"]:
            cid = generator["id"]
            directory = bundle / cid
            directory.mkdir(mode=0o700)
            work = home / (cid + "-generate")
            before = reconstruct(work, baseline, contents)
            argv = build_argv(work, generator["argv"], writable=True)
            validate_sandbox(argv, str(work), generator["argv"], True)
            spec = {"schema": campaign["schema"], "campaign": campaign, "candidate": cid,
                    "phase": "generate", "workspace": str(work), "argv": argv}
            gen = daemon.job(directory / "generate", spec)
            try:
                artifact = inspect_work(work, baseline, args.target, before)
                valid = True
            except (OSError, ValueError):
                artifact, valid = contents[args.target], False
            # Invalid generators still have a check phase, over baseline fallback,
            # explicitly ineligible in its bound spec; no unsigned skipped jobs.
            put(directory / "artifact", artifact)
            clean = home / (cid + "-check")
            before = reconstruct(clean, baseline, {**contents, args.target: artifact})
            argv = build_argv(clean, campaign["check"], writable=False)
            spec = {**spec, "phase": "check", "workspace": str(clean), "argv": argv,
                    "artifact_sha256": sha(artifact), "artifact_bytes": len(artifact),
                    "artifact_source": "generated-target" if valid else "baseline-fallback-ineligible",
                    "generation_valid": valid, "baseline_current": current(repo, campaign),
                    "generation_receipt_sha256": sha(read_file(directory / "generate",
                        "agent_receipts/" + gen["receipt_ref"])[0])}
            daemon.job(directory / "check", spec)
            require(inspect_work(clean, baseline, args.target, before) == artifact and
                    read_file(directory, "artifact")[0] == artifact, "postcheck artifact changed")
        require(current(repo, campaign), "source baseline invalidated before eligibility")
        put(bundle / "index.json", wire({"schema": campaign["schema"],
                                        "candidates": [g["id"] for g in campaign["generators"]]}))
        result, _ = verify_bundle(bundle, daemon.key, args.verifier)
        result.update(local_pubkey=daemon.key, identity="locally provisioned; not externally pinned")
        return result
    finally:
        if daemon:
            daemon.close()
        # Staging only, never the source or receipts/keys. Partial evidence stays.
        for cid in ("c0", "c1"):
            for phase in ("generate", "check"):
                path = home / (cid + "-" + phase)
                if path.exists():
                    shutil.rmtree(path)


class PublicationError(OSError):
    def __init__(self, error, publication):
        super().__init__(str(error))
        self.publication = dict(publication)


def publication_sync(fd, phase):
    """Name durability boundaries in diagnostics and fault-injection tests."""
    try:
        os.fsync(fd)
    except OSError as exc:
        raise OSError(exc.errno, phase + ": " + str(exc)) from exc


def publication_open(path):
    """Pin every directory component, not just the final path component."""
    path = Path(path)
    require(path.is_absolute() and ".." not in path.parts, "invalid publication directory")
    fd = os.open("/", os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC)
    try:
        for part in path.parts[1:]:
            child = os.open(part, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW |
                            os.O_CLOEXEC, dir_fd=fd)
            os.close(fd)
            fd = child
        return fd
    except BaseException:
        os.close(fd)
        raise


def publication_read(fd, name, mode):
    child = os.open(name, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK |
                    os.O_CLOEXEC, dir_fd=fd)
    try:
        before = os.fstat(child)
        require(stat.S_ISREG(before.st_mode) and stat.S_IMODE(before.st_mode) == mode
                and before.st_uid == os.geteuid() and before.st_nlink == 1
                and 0 <= before.st_size <= FILE_MAX, "invalid publication file: " + name)
        data = bytearray()
        while len(data) <= FILE_MAX:
            chunk = os.read(child, min(65536, FILE_MAX + 1 - len(data)))
            if not chunk:
                break
            data.extend(chunk)
        signature = lambda s: (s.st_dev, s.st_ino, s.st_size, s.st_mode, s.st_uid,
                               s.st_nlink, s.st_mtime_ns, s.st_ctime_ns)
        require(len(data) == before.st_size and signature(before) == signature(os.fstat(child))
                == signature(os.stat(name, dir_fd=fd, follow_symlinks=False)),
                "publication file changed: " + name)
        publication_sync(child, "read-" + name)
        return bytes(data)
    finally:
        os.close(child)


def publication_write(fd, name, data, mode):
    child = os.open(name, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW |
                    os.O_CLOEXEC, mode, dir_fd=fd)
    try:
        os.fchmod(child, mode)
        offset = 0
        while offset < len(data):
            written = os.write(child, data[offset:])
            require(written > 0, "short publication write")
            offset += written
        publication_sync(child, "write-" + name)
    finally:
        os.close(child)


def publish(args, result, artifact, *, recover=False):
    require(artifact is not None, "no eligible winner to publish")
    repo = Path(os.path.abspath(args.repo)) if recover else canonical(args.repo)
    require(str(repo) == result["campaign"]["repo"], "publication source context mismatch")
    destination = Path(os.path.abspath(args.destination))
    relative(destination.name)
    disjoint(destination, repo)
    disjoint(destination, canonical(args.bundle))
    identity = lambda s: [s.st_dev, s.st_ino]
    txn_name = ".autonomy-publication-" + sha(os.fsencode(destination.name))
    publication = {"state": "incomplete", "destination": str(destination),
                   "transaction": str(destination.parent / txn_name), "recovered": recover}

    def conflict(condition, message):
        if not condition:
            publication["state"] = "conflict"
            raise ValueError(message)

    parent = txn = lock = payload = -1
    verified = {k: result[k] for k in
                ("schema", "verified", "winner", "evidence", "campaign", "not_verified")}
    evidence_hash = sha(wire({k: verified[k] for k in ("campaign", "evidence", "winner")}))
    try:
        parent = publication_open(destination.parent)
        metadata = os.fstat(parent)
        require(metadata.st_uid == os.geteuid() and not metadata.st_mode & 0o022,
                "publication parent must be caller-owned and not writable by group/others")
        parent_id = identity(metadata)
        if not recover:
            conflict(not os.path.lexists(destination), "destination already exists")
            publication["state"] = "blocked"
            require(current(repo, result["campaign"]), "source baseline invalidated")
            os.mkdir(txn_name, mode=0o700, dir_fd=parent)
        txn = os.open(txn_name, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW |
                      os.O_CLOEXEC, dir_fd=parent)
        metadata = os.fstat(txn)
        require(metadata.st_uid == os.geteuid() and stat.S_IMODE(metadata.st_mode) == 0o700,
                "invalid protected transaction directory")
        publication["state"] = "incomplete"
        lock = os.open("lock", os.O_RDWR | os.O_NOFOLLOW | os.O_NONBLOCK | os.O_CLOEXEC |
                       (0 if recover else os.O_CREAT | os.O_EXCL), 0o600, dir_fd=txn)
        metadata = os.fstat(lock)
        require(stat.S_ISREG(metadata.st_mode) and metadata.st_nlink == 1 and
                metadata.st_uid == os.geteuid() and stat.S_IMODE(metadata.st_mode) == 0o600,
                "invalid transaction lock")
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        require(identity(metadata) == identity(os.stat("lock", dir_fd=txn, follow_symlinks=False)),
                "transaction lock replaced")
        if not recover:
            publication_sync(lock, "lock")
            publication_sync(txn, "claim-transaction")
            publication_sync(parent, "claim-parent")
            os.mkdir("payload", mode=0o700, dir_fd=txn)
            payload = os.open("payload", os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW |
                              os.O_CLOEXEC, dir_fd=txn)
            prepared = {"schema": "jash.autonomy.publication.v1", "id": os.urandom(16).hex(),
                        "destination": str(destination), "pubkey": args.pubkey.lower(),
                        "evidence_sha256": evidence_hash, "parent": parent_id,
                        "transaction": identity(os.fstat(txn)), "payload": identity(os.fstat(payload))}
        else:
            prepared = decode(publication_read(txn, "prepared.json", 0o600))
        fields(prepared, "schema id destination pubkey evidence_sha256 parent transaction payload")
        conflict(prepared["schema"] == "jash.autonomy.publication.v1" and
                type(prepared["id"]) is str and re.fullmatch("[0-9a-f]{32}", prepared["id"]) and
                prepared["destination"] == str(destination) and prepared["pubkey"] == args.pubkey.lower()
                and prepared["evidence_sha256"] == evidence_hash and prepared["parent"] == parent_id
                and prepared["transaction"] == identity(os.fstat(txn)) and
                type(prepared["payload"]) is list and len(prepared["payload"]) == 2 and
                all(type(v) is int and v >= 0 for v in prepared["payload"]),
                "publication intent does not match the supplied evidence, key or directory")
        manifest = wire({"schema": SCHEMA, "status": "done", "scope": "artifact-publication-only",
                         "pubkey": args.pubkey.lower(), "publication_id": prepared["id"],
                         "verification": verified})
        if not recover:
            publication_write(payload, "candidate", artifact, 0o444)
            publication_write(payload, "manifest.json", manifest, 0o444)
            publication_sync(payload, "prepare-payload")
            publication_write(txn, "prepared.json", wire(prepared), 0o600)
            publication_sync(txn, "prepare-transaction")
            publication_sync(parent, "prepare-parent")

        publication["state"] = "indeterminate"
        names = set(os.listdir(txn))
        staged = "payload" in names
        conflict(names == {"lock", "prepared.json"} | ({"payload"} if staged else set()),
                "unexpected transaction contents")
        if staged:
            conflict(not os.path.lexists(destination), "destination conflicts with staged publication")
            if payload < 0:
                payload = os.open("payload", os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW |
                                  os.O_CLOEXEC, dir_fd=txn)
        else:
            payload = os.open(destination.name, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW |
                              os.O_CLOEXEC, dir_fd=parent)
        metadata = os.fstat(payload)
        conflict(identity(metadata) == prepared["payload"] and metadata.st_uid == os.geteuid()
                and stat.S_IMODE(metadata.st_mode) in ((0o700,) if staged else (0o700, 0o555)),
                "output identity mismatch; identical foreign content is not ownership")
        conflict(set(os.listdir(payload)) == {"candidate", "manifest.json"} and
                publication_read(payload, "candidate", 0o444) == artifact and
                publication_read(payload, "manifest.json", 0o444) == manifest,
                "publication payload does not match verified bytes")
        def check_namespace():
            check_parent = publication_open(destination.parent)
            try:
                conflict(identity(os.fstat(check_parent)) == parent_id and
                         identity(os.stat(txn_name, dir_fd=parent, follow_symlinks=False)) == prepared["transaction"],
                         "publication namespace changed")
            finally:
                os.close(check_parent)

        check_namespace()

        if staged:
            publication["state"] = "prepared"
            # A crash can leave a complete prepared record before its directory
            # fsync. Establish all preparation durability before retrying rename.
            publication_sync(payload, "retry-prepare-payload")
            publication_sync(txn, "retry-prepare-transaction")
            publication_sync(parent, "retry-prepare-parent")
            require(current(repo, result["campaign"]), "source baseline invalidated before publication")
            conflict(identity(os.stat("payload", dir_fd=txn, follow_symlinks=False)) == prepared["payload"],
                    "staged payload replaced")
            rename = getattr(ctypes.CDLL(None, use_errno=True), "renameat2", None)
            require(rename is not None, "atomic no-replace publication unavailable")
            rename.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_uint]
            rename.restype = ctypes.c_int
            if rename(txn, b"payload", parent, os.fsencode(destination.name), 1) != 0:
                error = ctypes.get_errno()
                publication["state"] = "conflict" if error == errno.EEXIST else "prepared"
                raise OSError(error, os.strerror(error))
        # Reconcile an owned rename even if today's source is stale: completing
        # durability is not authorization to publish another candidate.
        publication["state"] = "indeterminate"
        conflict(identity(os.stat(destination.name, dir_fd=parent, follow_symlinks=False)) == prepared["payload"],
                "published output identity changed")
        # Moving a directory across parents needs owner write permission to
        # update '..'. Final read-only mode is part of commit, not preparation.
        os.fchmod(payload, 0o555)
        publication_sync(payload, "commit-payload")
        publication_sync(parent, "commit-parent")
        publication_sync(txn, "commit-transaction")
        check_namespace()
        conflict(identity(os.stat(destination.name, dir_fd=parent, follow_symlinks=False)) == prepared["payload"],
                 "published output identity changed")
        publication["state"] = "committed"
        publication["baseline_current"] = current(repo, result["campaign"])
    except (OSError, ValueError, KeyError, TypeError, RecursionError) as exc:
        if isinstance(exc, FileExistsError):
            publication["state"] = "conflict"
        raise PublicationError(exc, publication) from exc
    finally:
        # Never delete recovery evidence, even on preparation or I/O failures.
        for fd in (payload, lock, txn, parent):
            if fd >= 0:
                os.close(fd)
    return {**result, "destination": str(destination), "scope": "artifact-publication-only",
            "publication": publication}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    start = commands.add_parser("run", help="new private runtime HOME and new bundle required")
    for name in ("repo", "target", "check", "bundle", "runtime-home", "agentd", "verifier"):
        start.add_argument("--" + name, required=True)
    start.add_argument("--generate", action="append", required=True, help="JSON argv, at most twice")
    start.add_argument("--timeout-ms", type=int, default=2000)
    start.add_argument("--cgroup-root", help="opt in to v2 quotas using a trusted delegated cgroup-v2 root")
    start.add_argument("--resources", help="exact JSON resource profile; requires --cgroup-root")
    for verb in ("verify", "handoff", "publish", "promote", "recover"):
        sub = commands.add_parser(verb)
        for name in ("bundle", "pubkey", "verifier"):
            sub.add_argument("--" + name, required=True)
        if verb != "verify":
            sub.add_argument("--repo", required=verb in ("publish", "promote", "recover"))
        if verb in ("publish", "promote", "recover"):
            sub.add_argument("--destination", required=True)
    args = parser.parse_args()
    try:
        if args.command == "run":
            result = run(args)
        else:
            result, artifact = verify_bundle(args.bundle, args.pubkey, args.verifier)
            if args.command == "handoff":
                result["status"] = ("unknown" if args.repo is None else
                                    "current" if current(args.repo, result["campaign"]) else "invalidated")
                result["checks"] = result["campaign"]["check"]
            elif args.command in ("publish", "promote", "recover"):
                result = publish(args, result, artifact, recover=args.command == "recover")
        print(wire(result).decode(), end="")
        return 0
    except PublicationError as exc:
        print(wire({"verified": True, "error": str(exc), "publication": exc.publication}).decode(), end="")
        return 1
    except (OSError, ValueError, KeyError, TypeError, IndexError, RecursionError,
            subprocess.SubprocessError) as exc:
        result = {"verified": False, "error": str(exc)}
        if args.command == "handoff":
            result.update(status="invalidated", checks=[], not_verified=NOT_VERIFIED)
        print(wire(result).decode(), end="")
        return 1


if __name__ == "__main__":
    sys.exit(main())
