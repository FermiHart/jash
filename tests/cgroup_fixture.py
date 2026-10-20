"""Real, private cgroup-v2 delegation for resource/protocol tests.

No sudo/systemd calls, process migration, or shared subtree-control writes.
Callers must finish their supervisors before leaving delegated_root().
"""
from contextlib import contextmanager
import os
from pathlib import Path
import time
import uuid


class CgroupUnavailable(RuntimeError):
    """The test must report a skip (standalone runners use exit 77)."""


@contextmanager
def delegated_root():
    mount = Path("/sys/fs/cgroup")
    provided = os.environ.get("JASH_TEST_CGROUP_ROOT")
    if provided:
        candidates = [Path(provided).resolve()]
    else:
        try:
            relative = next(line[3:] for line in Path("/proc/self/cgroup").read_text().splitlines()
                            if line.startswith("0::"))
            current = mount / relative.lstrip("/")
            candidates = [current, *current.parents]
            candidates = [p for p in candidates if p == mount or mount in p.parents]
        except (OSError, StopIteration) as exc:
            raise CgroupUnavailable(f"cannot discover cgroup-v2 delegation: {exc}") from exc
    root = None
    failures = []
    for parent in candidates:
        private = None
        created = False
        try:
            if (parent / "cgroup.procs").read_text().strip():
                continue
            if not {"cpu", "memory", "pids"} <= set((parent / "cgroup.subtree_control").read_text().split()):
                continue
            private = parent / f"jash-test-{os.getpid()}-{uuid.uuid4().hex}"
            private.mkdir(mode=0o700)
            created = True
            (private / "cgroup.subtree_control").write_text("+cpu +memory +pids")
            root = private
            break
        except OSError as exc:
            failures.append(f"{parent}: {exc}")
            if created:
                private.rmdir()
    if root is None:
        raise CgroupUnavailable("no writable empty delegation with cpu/memory/pids already enabled"
                                + (": " + "; ".join(failures) if failures else ""))
    print(f"cgroup delegation: {root}", flush=True)
    try:
        yield root
    finally:
        # Only our new subtree is ever killed/removed, and rmdir only after
        # populated=0. Failure is an actual cleanup failure, never a green skip.
        (root / "cgroup.kill").write_text("1")
        deadline = time.monotonic() + 5
        while "populated 1" in (root / "cgroup.events").read_text():
            if time.monotonic() >= deadline:
                raise RuntimeError(f"private test cgroup remained populated: {root}")
            time.sleep(0.01)
        for directory, dirs, _ in os.walk(root, topdown=False):
            for name in dirs:
                child = Path(directory) / name
                if "populated 1" in (child / "cgroup.events").read_text():
                    raise RuntimeError(f"refusing to remove populated cgroup: {child}")
                child.rmdir()
        root.rmdir()
