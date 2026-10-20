#!/usr/bin/env python3
"""Pinned, serial master-suite runner (trusted registry, not a sandbox).

--list is JSONL: {index: 1-based integer, name: string, argv: string array}.
The manifest includes every registration, selection, and SHA-256 of captured
registry bytes. manifest_sha256 hashes canonical JSON (ASCII, sorted keys,
separators ','/':') with that one field omitted. Files are retained, including
private module copies and TMPDIR. A start without a result, or no summary,
means an incomplete run, never success. Consumers must require INCOMPLETE to
be absent and COMPLETE to bind the manifest and events digests. The fsynced
INCOMPLETE inode is atomically renamed to COMPLETE only after the summary.
Events are flushed/fsynced before
spawn and after completion; child stdout/stderr are unfiltered binary files.
Console progress has one flushed start/finish line per gate, with JSON-escaped,
abbreviated labels; complete labels and argv remain in the manifest and events.

Only registration bytes and module copies are pinned. Child scripts/binaries
remain live. Git metadata is a best-effort comparison hint, not an atomic tree
snapshot: the dirty fingerprint covers tracked diff and status (untracked
names, not their contents). HOME and PATH are preserved. Gates run in the
registry root; the manifest records that cwd and the separate invocation_cwd.
Shell startup hooks BASH_ENV and ENV are removed from gate execution.
Relative --output paths still resolve from the invocation directory. Private
TMPDIR isolates cooperating gates only; hardcoded /tmp and other shared writes
are not isolated. Exit-zero legacy SKIP text is not parsed as assertion coverage.
An exit-zero gate leaving live descendants is residual_processes, not success;
already-dead adopted zombies are reaped without failing the gate.

Execution requires Linux subreaper + libc/kernel pidfds + /proc. The runner and gate share
a private process group, so group INT/TERM reaches the runner even if a shell
defers signals while waiting. Start records include runner_pid and pgid. Only
the private CLI process owns reaping; no global pkill or group-only cleanup.
SIGKILL of the runner cannot be handled and may leave children; unmatched start
records expose it.
Process observations validate the caller's PID namespace and parse complete
bounded stat records (65,536 procfs entries, 4,096 descendants). Children forked
by other threads are included by PPID; the optional task/children file is not
required. Source errors and cleanup failures are explicit failed outcomes.
"""
import argparse
import ctypes
import errno
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import time

try:
    from . import proc_observation
except ImportError:
    import proc_observation


LIMITATIONS = [
    "Child test scripts and binaries are not frozen; only registry bytes and module copies are pinned.",
    "Exit-zero legacy SKIP output is not parsed and does not establish full assertion coverage.",
    "Private TMPDIR covers cooperating gates, not hardcoded /tmp or other shared writes; no sandbox.",
    "Runner artifacts are fd-anchored; child paths use the published name and are not rename-isolated.",
    "SIGKILL/crash may leave children; a missing result or summary means incomplete.",
    "Git fingerprint is non-atomic: tracked diff and status, not untracked file contents.",
]

LIBC = ctypes.CDLL(None, use_errno=True)
NATIVE_PIDFD_OPEN = getattr(os, "pidfd_open", None)
NATIVE_PIDFD_SEND_SIGNAL = getattr(signal, "pidfd_send_signal", None)


def pidfd_open(pid):
    if NATIVE_PIDFD_OPEN is not None:
        return NATIVE_PIDFD_OPEN(pid)
    function = getattr(LIBC, "pidfd_open", None)
    if function is None:
        raise OSError(errno.ENOSYS, "libc has no pidfd_open")
    result = function(pid, 0)
    if result < 0:
        error = ctypes.get_errno()
        raise OSError(error, os.strerror(error))
    return result


def pidfd_send_signal(fd, sig):
    if NATIVE_PIDFD_SEND_SIGNAL is not None:
        NATIVE_PIDFD_SEND_SIGNAL(fd, sig)
        return
    function = getattr(LIBC, "pidfd_send_signal", None)
    if function is None:
        raise OSError(errno.ENOSYS, "libc has no pidfd_send_signal")
    if function(fd, sig, None, 0) != 0:
        error = ctypes.get_errno()
        raise OSError(error, os.strerror(error))


def pidfd_for(handles, pid):
    if pid in handles:
        try:
            pidfd_send_signal(handles[pid], 0)
            return handles[pid]
        except ProcessLookupError:
            os.close(handles.pop(pid))
    fd = pidfd_open(pid)
    handles[pid] = fd
    return fd


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode()


def durable(file, data):
    file.write(data)
    file.flush()
    os.fsync(file.fileno())


def file_sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as file:
        for chunk in iter(lambda: file.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def same_file(fd, path, *, dir_fd=None, follow_symlinks=False):
    try:
        target = os.stat(path, dir_fd=dir_fd, follow_symlinks=follow_symlinks)
    except OSError:
        return False
    held = os.fstat(fd)
    return (held.st_dev, held.st_ino) == (target.st_dev, target.st_ino)


def create_output(requested):
    """Exclusively create and pin the artifact directory before path resolution."""
    flags = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC
    if requested is None:
        display = Path(tempfile.mkdtemp(prefix="jash-suite-"))
        directory_fd = os.open(display, flags)
        if not same_file(directory_fd, display):
            os.close(directory_fd)
            raise RuntimeError("default --output changed while being opened")
        display = display.resolve(strict=True)
    else:
        parent = requested.parent.resolve(strict=True)
        parent_fd = os.open(parent, flags)
        directory_fd = None
        try:
            if not same_file(parent_fd, parent):
                raise RuntimeError("--output parent changed while being opened")
            os.mkdir(requested.name, mode=0o700, dir_fd=parent_fd)
            directory_fd = os.open(requested.name, flags, dir_fd=parent_fd)
            if not same_file(directory_fd, requested.name, dir_fd=parent_fd):
                raise RuntimeError("--output changed while being opened")
            display = parent / requested.name
        except Exception:
            if directory_fd is not None:
                os.close(directory_fd)
            raise
        finally:
            os.close(parent_fd)
    try:
        os.fchmod(directory_fd, 0o700)
        anchor = Path(f"/proc/{os.getpid()}/fd/{directory_fd}")
        if not same_file(directory_fd, anchor, follow_symlinks=True):
            raise RuntimeError("cannot anchor --output through procfs")
    except Exception:
        os.close(directory_fd)
        raise
    return display, anchor, directory_fd


def commit_evidence(output, directory_fd, incomplete_file, manifest_sha256):
    if not same_file(incomplete_file.fileno(), "INCOMPLETE", dir_fd=directory_fd):
        raise RuntimeError("INCOMPLETE marker changed during the run")
    marker = canonical(dict(version=1, status="committed",
                            manifest_sha256=manifest_sha256,
                            events_sha256=file_sha256(output / "events.jsonl"))) + b"\n"
    incomplete_file.seek(0)
    incomplete_file.truncate()
    durable(incomplete_file, marker)
    incomplete_file.close()
    os.replace("INCOMPLETE", "COMPLETE", src_dir_fd=directory_fd, dst_dir_fd=directory_fd)
    try:
        os.fsync(directory_fd)
    except OSError as error:
        # COMPLETE names an already-fsynced inode and binds already-fsynced
        # evidence. A crash can conservatively lose the rename, never expose
        # uncommitted bytes as COMPLETE.
        return str(error)
    return None


def enumerate_registry(source):
    env = dict(os.environ)
    # Noninteractive Bash startup hooks must not run before the exporter.
    for key in ("BASH_ENV", "ENV", "JASH_SUITE_PINNED"):
        env.pop(key, None)
    exported = subprocess.run(["bash", "--noprofile", "--norc", "-s", "--", "--suite-export-v1"],
                              input=source, capture_output=True, env=env, timeout=30)
    if exported.returncode:
        raise ValueError(f"registry export failed (exit {exported.returncode}): "
                         + os.fsdecode(exported.stderr[:4096]))
    fields = exported.stdout.split(b"\0")
    if fields.pop() != b"":
        raise ValueError("registry export is not NUL terminated")
    gates = []
    pos = 0
    while pos < len(fields):
        count = fields[pos]
        if not count.isdigit() or len(count) > 9:
            raise ValueError("invalid registry field count")
        count = int(count)
        pos += 1
        if count < 2 or pos + count > len(fields):
            raise ValueError("registry requires a label and nonempty argv")
        name, *argv = [os.fsdecode(s) for s in fields[pos:pos + count]]
        if not argv[0]:
            raise ValueError("registry executable must not be empty")
        gates.append(dict(index=len(gates) + 1, name=name, argv=argv))
        pos += count
    return gates


def subreaper(enable):
    if sys.platform != "linux":
        raise RuntimeError("execution requires Linux subreaper, /proc and libc/kernel pidfd support; --list is portable")
    previous = ctypes.c_int()
    if LIBC.prctl(37, ctypes.byref(previous), 0, 0, 0) or LIBC.prctl(36, int(enable), 0, 0, 0):
        raise OSError(ctypes.get_errno(), "cannot configure child subreaper")
    return previous.value


def descendants():
    """Only the CLI runner's descendants, including children in nested sessions."""
    own = os.getpid()
    pending = [own]
    found = set()
    while pending:
        parent = pending.pop()
        try:
            children = proc_observation.direct_children(parent, all_threads=True)
        except (FileNotFoundError, ProcessLookupError):
            if parent == own:
                raise
            continue  # It vanished; its already-recorded PID keeps cleanup active.
        for pid in children:
            if pid != own and pid not in found:
                if len(found) >= 4096:
                    raise proc_observation.ProcObservationError("descendant tree exceeds 4096 processes")
                found.add(pid)
                pending.append(pid)
    return found


def reap_adopted_zombies(exclude):
    """Reap only direct adopted zombies; Popen retains its own wait status."""
    own = os.getpid()
    direct = proc_observation.direct_children(own, all_threads=True)
    for pid in direct:
        if pid == exclude:
            continue
        try:
            observed = proc_observation.read_process_stat(pid)
            if observed.state not in (b"Z", b"X", b"x") or observed.parent != own:
                continue
            os.waitpid(pid, os.WNOHANG)
        except (FileNotFoundError, ProcessLookupError, ChildProcessError):
            continue


def still_descendant(pid):
    # Revalidate after pidfd_open: /proc enumeration can race PID reuse.
    seen = set()
    while pid > 1 and pid not in seen:
        if pid == os.getpid():
            return True
        seen.add(pid)
        try:
            pid = proc_observation.read_process_stat(pid).parent
        except (FileNotFoundError, ProcessLookupError):
            return False
    return False


def cleanup_tree(process):
    """Bounded TERM/KILL/reap, with pidfds so no recycled PID is signaled."""
    handles = {}
    started = time.monotonic()
    signaled = set()
    try:
        while True:
            process.poll()  # Popen owns its wait status; never waitpid(-1).
            pids = descendants()
            if process.returncode is None:
                pids.add(process.pid)
            if not pids:
                # Enumeration is not atomic with orphan adoption.  Only ECHILD
                # proves the subreaper has no remaining child.  WNOWAIT leaves
                # Popen's leader status under Popen's ownership.
                try:
                    dead = os.waitid(os.P_ALL, 0, os.WEXITED | os.WNOHANG | os.WNOWAIT)
                except ChildProcessError:
                    return dict(signaled_descendants=len(signaled), cleanup_error=None)
                if dead is not None and dead.si_pid != process.pid:
                    try:
                        os.waitpid(dead.si_pid, os.WNOHANG)
                    except ChildProcessError:
                        pass
            elapsed = time.monotonic() - started
            for pid in pids:
                try:
                    fd = pidfd_for(handles, pid)
                except ProcessLookupError:
                    continue
                if not still_descendant(pid):
                    os.close(handles.pop(pid))
                    continue
                try:
                    state = proc_observation.read_process_stat(pid).state
                    if not still_descendant(pid):
                        continue
                    # A pidfd signal can succeed for a zombie. Count only
                    # observed-live targets, and always signal via the pidfd.
                    if state not in (b"Z", b"X", b"x"):
                        if pid not in signaled:
                            pidfd_send_signal(handles[pid], signal.SIGTERM)
                            signaled.add(pid)
                        if elapsed >= .3:
                            pidfd_send_signal(handles[pid], signal.SIGKILL)
                except (FileNotFoundError, ProcessLookupError):
                    continue
                if pid != process.pid:
                    try:
                        reaped, _ = os.waitpid(pid, os.WNOHANG)
                        if reaped:
                            os.close(handles.pop(pid))
                    except ChildProcessError:
                        pass  # Not yet adopted; its parent still owns the wait.
            # Close dead handles so a newly recycled *descendant* PID can be
            # acquired independently next round (never signal by numeric PID).
            for pid in list(handles):
                if pid not in pids:
                    os.close(handles.pop(pid))
            if elapsed >= 3:
                return dict(signaled_descendants=len(signaled),
                            cleanup_error="descendants remain after bounded TERM/KILL/reap")
            time.sleep(.02)
    except (OSError, RuntimeError) as error:
        return dict(signaled_descendants=len(signaled), cleanup_error=f"process observation or cleanup failed: {error}")
    finally:
        for fd in handles.values():
            os.close(fd)


def git_metadata(root, output):
    metadata = {"head": None, "dirty_sha256": None}
    try:
        head = subprocess.run(["git", "-C", str(root), "rev-parse", "HEAD"],
                              stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, timeout=5, check=True)
        metadata["head"] = head.stdout.decode("ascii").strip()
        digest = hashlib.sha256()
        with tempfile.TemporaryFile(dir=output) as spool:
            for args in (["diff", "--no-ext-diff", "--no-textconv", "--binary", "HEAD", "--"],
                         ["status", "--porcelain=v1", "-z", "--untracked-files=all"]):
                subprocess.run(["git", "-C", str(root), *args], stdout=spool,
                               stderr=subprocess.DEVNULL, timeout=10, check=True,
                               env=dict(os.environ, GIT_OPTIONAL_LOCKS="0"))
            spool.seek(0)
            for chunk in iter(lambda: spool.read(1024 * 1024), b""):
                digest.update(chunk)
        metadata["dirty_sha256"] = digest.hexdigest()
    except (OSError, subprocess.SubprocessError, UnicodeError):
        metadata["error"] = "git comparison metadata unavailable"
    return metadata


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--list", action="store_true", help="JSONL index/name/argv; no artifacts or gate setup")
    parser.add_argument("--from", dest="first", type=int, default=1)
    parser.add_argument("--to", dest="last", type=int)
    parser.add_argument("--timeout", type=float, default=600, help="finite positive seconds per gate (default: 600)")
    parser.add_argument("--output", type=Path, help="exclusively create NEWDIR, mode 0700")
    parser.add_argument("--registry", type=Path, default=Path(__file__).resolve().parents[1] / "tests/test_suite.sh",
                        help="trusted registry fixture; default is the fixed repository master")
    args = parser.parse_args(argv)
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("--timeout must be finite and positive")
    if args.first < 1 or (args.last is not None and args.last < args.first):
        parser.error("invalid or reversed range")
    try:
        # Read once. No shell syntax heuristics, eval, or temporary script.
        registry = args.registry.resolve(strict=True)
        source = registry.read_bytes()
        gates = enumerate_registry(source)
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        parser.error(str(error))
    last = len(gates) if args.last is None else args.last
    if not gates or args.first > last or last > len(gates):
        parser.error("range is empty or outside the registry")
    selected = gates[args.first - 1:last]
    if args.list:
        for gate in selected:
            print(canonical(gate).decode("ascii"))
        return 0

    # Root/module/artifact setup happens only after full CLI/registry validation.
    root = registry.parent.parent
    invocation_cwd = os.getcwd()
    interrupted = 0
    previous_handlers = {}
    previous_subreaper = None
    previous_group = None
    output = None
    output_display = None
    events = None
    directory_fd = None
    incomplete_file = None
    manifest = None
    active = None
    results = []
    runner_error = None
    finalizing = False

    def stop(sig, frame):
        nonlocal interrupted
        if not interrupted and not finalizing:
            interrupted = sig

    def event(record):
        offset = events.tell()
        try:
            durable(events, canonical(record) + b"\n")
        except OSError:
            # A flushed but non-durable success record is worse than no record.
            try:
                events.seek(offset)
                events.truncate()
                events.flush()
                os.fsync(events.fileno())
            except OSError:
                pass
            raise

    try:
        previous_subreaper = subreaper(True)
        # Fail closed if the kernel or procfs cannot support subtree cleanup.
        fd = pidfd_open(os.getpid())
        os.close(fd)
        proc_observation.direct_children(os.getpid(), all_threads=True)
        for sig in (signal.SIGINT, signal.SIGTERM):
            previous_handlers[sig] = signal.signal(sig, stop)
        previous_group = os.getpgrp()
        if previous_group != os.getpid():
            os.setpgid(0, 0)
        output_display, output, directory_fd = create_output(args.output)
        print(f"suite artifacts: {output_display}", flush=True)
        modules = output / "modules"
        scratch = output / "tmp"
        modules_display = output_display / "modules"
        scratch_display = output_display / "tmp"
        modules.mkdir(mode=0o700)
        scratch.mkdir(mode=0o700)
        for module in sorted((root / "modules").glob("*/*.modjash")):
            destination = modules / module.parent.name
            destination.mkdir(mode=0o700, exist_ok=True)
            with module.open("rb") as src, (destination / module.name).open("xb") as dst:
                os.fchmod(dst.fileno(), 0o600)
                shutil.copyfileobj(src, dst)
        env = dict(os.environ)
        for key in ("BASH_ENV", "ENV", "JASH_SUITE_PINNED", "JASH_SUITE_ROOT",
                    "JASH_SUITE_MODULE_ROOT"):
            env.pop(key, None)
        env.update(JASH_GUARD_BYPASS="0", JASH_GUARD_BYPASS_STRICT="0", JASH_SIGIL="0",
                   JASH_MODULE_DEV_ROOT=str(modules_display), TMPDIR=str(scratch_display))
        manifest = dict(version=1, source=dict(path=str(registry),
                                             sha256=hashlib.sha256(source).hexdigest()),
                        gates=gates, selected=[g["index"] for g in selected], timeout=args.timeout,
                        cwd=str(root), invocation_cwd=invocation_cwd,
                        git=git_metadata(root, output), limitations=LIMITATIONS,
                        process_boundary="Linux private process group + subreaper + checked /proc PID/PPID/start-time descendants + pidfds",
                        environment={key: env.get(key) for key in
                                     ("HOME", "PATH", "TMPDIR", "JASH_MODULE_DEV_ROOT", "JASH_SIGIL",
                                      "JASH_GUARD_BYPASS", "JASH_GUARD_BYPASS_STRICT")})
        manifest["manifest_sha256"] = hashlib.sha256(canonical(manifest)).hexdigest()
        incomplete_file = (output / "INCOMPLETE").open("xb+")
        durable(incomplete_file, b"suite evidence is not committed\n")
        for name, data in (("registry.sh", source),
                           ("manifest.json", canonical(manifest) + b"\n")):
            with (output / name).open("xb") as file:
                durable(file, data)
        events = (output / "events.jsonl").open("xb")
        # Persist directory entries too, before any gate can start.
        os.fsync(directory_fd)
        for gate in selected:
            if interrupted:
                break
            paths = dict(stdout=f"{gate['index']:04d}.stdout", stderr=f"{gate['index']:04d}.stderr")
            record = dict(index=gate["index"], name=gate["name"], argv=gate["argv"], **paths)
            label = gate["name"][:160] + ("..." if len(gate["name"]) > 160 else "")
            progress = dict(index=gate["index"], name=label)
            began = time.monotonic()
            timed_out = False
            spawn_error = None
            cleanup = dict(signaled_descendants=0, cleanup_error=None)
            rc = None
            pid = None
            with (output / paths["stdout"]).open("xb") as stdout, (output / paths["stderr"]).open("xb") as stderr:
                os.fsync(directory_fd)
                event(dict(event="start", **record, monotonic=began, time=time.time(),
                           runner_pid=os.getpid(), pgid=os.getpgrp(),
                           manifest_sha256=manifest["manifest_sha256"]))
                print("suite start " + canonical(progress).decode("ascii"), flush=True)
                if not interrupted:
                    try:
                        deadline = time.monotonic() + args.timeout
                        active = subprocess.Popen(gate["argv"], stdin=subprocess.DEVNULL,
                                                  stdout=stdout, stderr=stderr, env=env, cwd=root)
                        pid = active.pid
                    except OSError as error:
                        spawn_error = str(error)
                    if active is not None:
                        try:
                            while active.poll() is None and not interrupted:
                                reap_adopted_zombies(active.pid)
                                if time.monotonic() >= deadline:
                                    timed_out = True
                                    break
                                time.sleep(.02)
                        finally:
                            cleanup = cleanup_tree(active)
                            rc = active.poll()
                            if not cleanup["cleanup_error"]:
                                active = None
                stdout.flush()
                stderr.flush()
                os.fsync(stdout.fileno())
                os.fsync(stderr.fileno())
            status = ("interrupted" if interrupted else "timed_out" if timed_out else
                      "spawn_error" if spawn_error else
                      "cleanup_failed" if cleanup["cleanup_error"] else
                      "residual_processes" if rc == 0 and cleanup["signaled_descendants"] else
                      "exited_zero" if rc == 0 else
                      "skipped" if rc == 77 else "signaled" if rc is not None and rc < 0 else "failed")
            result = dict(event="result", **record, status=status, returncode=rc, pid=pid,
                          signal=-rc if rc is not None and rc < 0 else None, timeout=timed_out,
                          duration=time.monotonic() - began, spawn_error=spawn_error, **cleanup)
            event(result)
            results.append(result)
            print("suite finish " + canonical(dict(progress, status=status, returncode=rc,
                  duration=round(result["duration"], 3))).decode("ascii"), flush=True)
            if cleanup["cleanup_error"]:
                raise RuntimeError(cleanup["cleanup_error"])
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        runner_error = str(error)
        print(f"suite runner error: {error}", file=sys.stderr)
    finally:
        # Never remove artifacts or module/TMPDIR state, especially while a
        # child could still be alive. Handle active trees even on write errors.
        if active is not None:
            try:
                cleanup = cleanup_tree(active)
                if cleanup["cleanup_error"]:
                    runner_error = cleanup["cleanup_error"]
            except (OSError, RuntimeError) as error:
                runner_error = f"descendant cleanup failed: {error}"
        # Once every selected result is durable, later terminal signals do not
        # retroactively turn a completed run into an interrupted contradiction.
        finalizing = True
        if (directory_fd is not None and output_display is not None
                and not same_file(directory_fd, output_display)):
            runner_error = "--output no longer names the pinned artifact directory"
            print(f"suite runner error: {runner_error}", file=sys.stderr)
        zero = (not runner_error and not interrupted and len(results) == len(selected)
                and all(r["status"] == "exited_zero" for r in results))
        summary = dict(event="summary", status="interrupted" if interrupted else
                       "runner_error" if runner_error else "completed", registered=len(gates),
                       selected=len(selected), completed=len(results), unselected=len(gates) - len(selected),
                       failed=sum(r["status"] in ("failed", "spawn_error", "signaled", "timed_out",
                                                 "residual_processes", "cleanup_failed") for r in results),
                       residual_processes=sum(r["status"] == "residual_processes" for r in results),
                       timed_out=sum(r["timeout"] for r in results),
                       interrupted=sum(r["status"] == "interrupted" for r in results),
                       skipped=sum(r["status"] == "skipped" for r in results),
                       signal=interrupted or None, runner_error=runner_error,
                       selected_exited_zero=bool(zero),
                       all_registered_exited_zero=bool(zero and len(selected) == len(gates)),
                       limitations=LIMITATIONS)
        if events is not None:
            summary_durable = False
            try:
                event(summary)
                summary_durable = True
            except OSError as error:
                runner_error = str(error)
                summary.update(status="interrupted" if interrupted else "runner_error",
                               runner_error=runner_error, selected_exited_zero=False,
                               all_registered_exited_zero=False)
                print(f"suite summary write failed: {error}", file=sys.stderr)
            finally:
                try:
                    events.close()
                except OSError as error:
                    summary_durable = False
                    runner_error = f"suite event close failed: {error}"
                    summary.update(status="interrupted" if interrupted else "runner_error",
                                   runner_error=runner_error, selected_exited_zero=False,
                                   all_registered_exited_zero=False)
                    print(runner_error, file=sys.stderr)
            if (summary_durable and incomplete_file is not None
                    and directory_fd is not None and manifest is not None):
                try:
                    commit_warning = commit_evidence(output, directory_fd, incomplete_file,
                                                     manifest["manifest_sha256"])
                    if commit_warning:
                        print("suite commit directory fsync failed after atomic publish; "
                              f"a crash may conservatively restore INCOMPLETE: {commit_warning}",
                              file=sys.stderr)
                except OSError as error:
                    runner_error = f"suite evidence commit failed: {error}"
                    summary.update(status="interrupted" if interrupted else "runner_error",
                                   runner_error=runner_error, selected_exited_zero=False,
                                    all_registered_exited_zero=False)
                    print(runner_error, file=sys.stderr)
                except RuntimeError as error:
                    runner_error = f"suite evidence commit failed: {error}"
                    summary.update(status="interrupted" if interrupted else "runner_error",
                                   runner_error=runner_error, selected_exited_zero=False,
                                   all_registered_exited_zero=False)
                    print(runner_error, file=sys.stderr)
        if incomplete_file is not None and not incomplete_file.closed:
            try:
                incomplete_file.close()
            except OSError as error:
                runner_error = f"suite marker close failed: {error}"
                print(runner_error, file=sys.stderr)
        if directory_fd is not None:
            os.close(directory_fd)
        for sig, handler in previous_handlers.items():
            signal.signal(sig, handler)
        if previous_subreaper is not None:
            subreaper(previous_subreaper)
        if previous_group is not None and previous_group != os.getpgrp():
            try:
                os.setpgid(0, previous_group)
            except OSError:
                pass  # The original caller's group may no longer exist.
    if events is not None:
        print(canonical(summary).decode("ascii"))
    return 128 + interrupted if interrupted else 1 if runner_error or not zero else 0


if __name__ == "__main__":
    sys.exit(main())
