"""Strict Linux process observations shared by runtime regression fixtures.

Some Linux hosts omit task/PID/children.  In that case a bounded scan of
PID/stat supplies the same direct-parent relationship.  An unreadable or
malformed source is an error, never an empty child list.  This is a momentary
observation for test assertions; it is not, by itself, proof of quiescence.
"""

from dataclasses import dataclass, field
import errno
import os
from pathlib import Path
import re


class ProcObservationError(RuntimeError):
    """The process relationship could not be observed completely."""


@dataclass(frozen=True)
class ProcessStat:
    pid: int
    parent: int
    start: int
    state: bytes = field(default=b"S", compare=False)


_PID_MAX = (1 << 31) - 1
_STAT_BYTES_MAX = 4096
_CHILDREN_BYTES_MAX = 65536
_UNAVAILABLE = {errno.ENOENT, errno.ENOTDIR, errno.ENOSYS, errno.ENOTSUP}
_VANISHED = {errno.ENOENT, errno.ESRCH}


def _read_bounded(directory: int, path: str, limit: int) -> bytes:
    fd = os.open(path, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW | os.O_NONBLOCK,
                 dir_fd=directory)
    try:
        chunks = []
        size = 0
        while True:
            part = os.read(fd, min(4096, limit + 1 - size))
            if not part:
                return b"".join(chunks)
            size += len(part)
            if size > limit:
                raise ProcObservationError(f"{path}: observation exceeds {limit} bytes")
            chunks.append(part)
    finally:
        os.close(fd)


def _stat(directory: int, path: str, expected_pid: int) -> ProcessStat:
    raw = _read_bounded(directory, path, _STAT_BYTES_MAX)
    prefix = str(expected_pid).encode("ascii") + b" ("
    close = raw.rfind(b")")
    if (not raw.startswith(prefix) or close < len(prefix) or b"\0" in raw
            or not raw.endswith(b"\n") or raw[close:close + 2] != b") "):
        raise ProcObservationError(f"{path}: malformed stat or mismatched PID")
    # The comm field may itself contain spaces, newlines and closing brackets.
    # All fields following its final ')' are numeric except the one-byte state.
    tail = raw[close + 2:-1].split(b" ")
    if (len(tail) < 20 or not re.fullmatch(rb"[A-Za-z]", tail[0])
            or any(not re.fullmatch(rb"-?[0-9]+", part) for part in tail[1:])):
        raise ProcObservationError(f"{path}: incomplete or malformed stat fields")
    parent, start = int(tail[1]), int(tail[19])
    if not 0 <= parent <= _PID_MAX or not 0 <= start < 1 << 64:
        raise ProcObservationError(f"{path}: invalid parent or process identity")
    return ProcessStat(expected_pid, parent, start, tail[0])


def _possibly_live_stat(directory: int, pid: int) -> ProcessStat | None:
    try:
        return _stat(directory, f"{pid}/stat", pid)
    except OSError as error:
        if error.errno in _VANISHED:
            return None
        raise


def _check_namespace(directory: int) -> None:
    caller = _stat(directory, "self/stat", os.getpid())
    if caller.parent != os.getppid():
        raise ProcObservationError("procfs does not match the caller's PID namespace")


def read_process_stat(pid: int, *, proc_root: Path = Path("/proc")) -> ProcessStat:
    """Read identity, parent and state without treating any source error as absence."""
    if type(pid) is not int or not 0 < pid <= _PID_MAX:
        raise ValueError("pid must be a positive Linux PID")
    directory = os.open(proc_root, os.O_RDONLY | os.O_CLOEXEC | os.O_DIRECTORY | os.O_NOFOLLOW)
    try:
        _check_namespace(directory)
        return _stat(directory, f"{pid}/stat", pid)
    finally:
        os.close(directory)


def _scan_children(directory: int, pid: int, max_entries: int,
                   max_children: int) -> dict[int, ProcessStat]:
    found = {}
    with os.scandir(directory) as entries:
        count = 0
        for entry in entries:
            count += 1
            if count > max_entries:
                raise ProcObservationError("procfs enumeration exceeds its entry limit")
            name = entry.name
            if not re.fullmatch(r"[0-9]+", name):
                continue
            child = int(name)
            if not 0 < child <= _PID_MAX or str(child) != name:
                raise ProcObservationError("procfs contains an invalid numeric process entry")
            observed = _possibly_live_stat(directory, child)
            if observed is not None and observed.parent == pid:
                if len(found) >= max_children:
                    raise ProcObservationError("direct-child observation exceeds its child limit")
                found[child] = observed
    return found


def direct_children(pid: int, *, proc_root: Path = Path("/proc"),
                    max_entries: int = 65536, max_children: int = 4096,
                    all_threads: bool = False) -> list[int]:
    """Return observed direct children, preserving source/identity failures.

    The caller's PID namespace must match procfs.  The target's PID, parent and
    start time must remain unchanged during collection.  Only processes that
    vanish while their stat is read are omitted; access errors are propagated.
    all_threads uses the process PPID source directly, covering children forked
    by any thread, which one task/PID/children file cannot establish.  The
    optional proc_root and bounds support deterministic negative fixtures.
    """
    if type(pid) is not int or not 0 < pid <= _PID_MAX:
        raise ValueError("pid must be a positive Linux PID")
    if max_entries < 1 or max_children < 0:
        raise ValueError("invalid process observation limits")
    directory = os.open(proc_root, os.O_RDONLY | os.O_CLOEXEC | os.O_DIRECTORY | os.O_NOFOLLOW)
    try:
        _check_namespace(directory)
        target = _stat(directory, f"{pid}/stat", pid)
        path = f"{pid}/task/{pid}/children"
        found: dict[int, ProcessStat] = {}
        try:
            raw = None if all_threads else _read_bounded(directory, path, _CHILDREN_BYTES_MAX)
        except OSError as error:
            if error.errno not in _UNAVAILABLE:
                raise
            raw = None
        if raw is None:
            found = _scan_children(directory, pid, max_entries, max_children)
        else:
            if not re.fullmatch(rb"(?:[1-9][0-9]*[ \t\n]*)*", raw):
                raise ProcObservationError(f"{path}: malformed child list")
            listed = [int(token) for token in raw.split()]
            if len(listed) > max_children or len(set(listed)) != len(listed):
                raise ProcObservationError(f"{path}: duplicate children or child limit exceeded")
            for child in listed:
                if child > _PID_MAX:
                    raise ProcObservationError(f"{path}: child PID exceeds Linux range")
                observed = _possibly_live_stat(directory, child)
                if observed is not None:
                    if observed.parent != pid:
                        raise ProcObservationError(f"{path}: child identity changed during observation")
                    found[child] = observed
        if _stat(directory, f"{pid}/stat", pid) != target:
            raise ProcObservationError("target identity changed during child observation")
        stable = []
        for child, observed in found.items():
            current = _possibly_live_stat(directory, child)
            if current is not None:
                if current != observed:
                    raise ProcObservationError("child identity changed during observation")
                stable.append(child)
        return sorted(stable)
    finally:
        os.close(directory)
