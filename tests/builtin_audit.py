#!/usr/bin/env python3
"""Execute a semantic, host-isolated probe for every registered Jash builtin.

The builtin catalog is deliberately loaded from ``build/jash --builtins-json``.
Every registered name must have an explicit probe below.  A probe may exercise
an expected parser/guardrail rejection, but it may not be help-only or absent.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
import resource
import shlex
import signal
import subprocess
import sys
import tempfile
import threading
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable


DEFAULT_TIMEOUT = 2.0
MAX_OUTPUT = 64 * 1024
MAX_FILE_BYTES = 2 * 1024 * 1024
SAFE_PATH = "/usr/bin:/bin"


@dataclass(frozen=True)
class Fixture:
    """One command-line probe, safe because it uses only the private case dir."""

    label: str
    argv: tuple[str, ...]
    expected_exit: int
    stdin: str = ""
    prelude: tuple[str, ...] = ()
    smoke_only: bool = False
    script: str | None = None
    expected_stop: bool = False
    guarded: bool = False


@dataclass
class RunResult:
    label: str
    exit_code: int | None
    timed_out: bool
    crash: dict[str, Any] | None
    stdout: str
    stderr: str
    mutations: list[dict[str, str]]
    descendants: list[dict[str, Any]]
    expected_exit: int
    passed: bool
    executed: bool
    expected_stop: bool
    suspended: bool
    elapsed: float


def fixture(label: str, *argv: str, expected: int = 0, stdin: str = "",
            prelude: Iterable[str] = (), smoke_only: bool = False,
            script: str | None = None, expected_stop: bool = False,
            guarded: bool = False) -> Fixture:
    return Fixture(label, tuple(argv), expected, stdin, tuple(prelude), smoke_only,
                   script, expected_stop, guarded)


# This is an allowlist, not a heuristic.  In particular, a read-looking name
# is not enough to make a command eligible for execution.
FIXTURES: dict[str, tuple[Fixture, ...]] = {
    # Shell control and state.  The script forms deliberately put control-flow
    # builtins in the context where their contract is meaningful.
    "cd": (fixture("private-directory", "cd", "private-dir"),),
    "pwd": (fixture("positive", "pwd"),),
    "exit": (fixture("controlled-exit", "exit", "0"),),
    ":": (fixture("positive", ":"),),
    "true": (fixture("positive", "true"),),
    "false": (fixture("negative", "false", expected=1),),
    "echo": (fixture("positive", "echo", "audit-echo"),),
    "printf": (fixture("positive", "printf", "%s\\n", "audit-printf"),),
    "help": (fixture("positive", "help", "echo"),),
    "version": (fixture("positive", "version"),),
    "uname": (fixture("positive", "uname"),),
    "osif": (fixture("capability-report", "osif", "--json"),),
    "undo": (fixture("dry-run", "undo", "--dry", prelude=("cd private-dir",)),),
    "redo": (fixture("guardrail", "redo", expected=1, guarded=True),),
    "why": (fixture("diagnostic", "why"),),
    "resume": (fixture("handoff-check", "resume"),),
    "assert": (fixture("checked-assertion", "assert", "audit", "--check", "true"),),
    "timeline": (fixture("history-read", "timeline"),),
    "blame-file": (fixture("private-file", "blame-file", "--json", "fixture.txt", expected=2),),
    "distill": (fixture("explain-state", "distill", "audit-recipe", "--explain", expected=2),),
    "recipe": (fixture("list-recipes", "recipe", "list"),),
    "type": (
        fixture("positive", "type", "echo"),
        fixture("negative", "type", "__jash_audit_missing__", expected=1),
    ),
    "which": (
        fixture("positive", "which", "echo"),
        fixture("negative", "which", "__jash_audit_missing__", expected=1),
    ),
    "test": (
        fixture("positive", "test", "-n", "audit"),
        fixture("negative", "test", "-z", "audit", expected=1),
    ),
    "[": (
        fixture("positive", "[", "1", "-eq", "1", "]"),
        fixture("negative", "[", "1", "-eq", "2", "]", expected=1),
    ),
    # In non-interactive `-c` mode the directory stack is not shared with a
    # preceding command, so the safe negative contract is the empty-stack
    # diagnostic rather than pretending that a positive push/pop exists.
    "pushd": (fixture("private-directory-stack", "pushd", "subdir"),),
    "popd": (fixture("private-directory-stack", "popd", expected=1),),
    "dirs": (fixture("private-directory-stack", "dirs"),),
    "read": (fixture("safe-shell-state", "read", "JASH_AUDIT_INPUT", stdin="value\n"),),
    "declare": (fixture("safe-shell-state", "declare", "JASH_AUDIT=value"),),
    "clear": (fixture("safe-terminal", "clear"),),
    "env": (fixture("positive", "env"),),
    "export": (fixture("safe-shell-state", "export", "JASH_AUDIT=value"),),
    "unset": (fixture("safe-shell-state", "unset", "JASH_AUDIT"),),
    "alias": (fixture("safe-shell-state", "alias", "audit_alias=echo audit"),),
    "unalias": (fixture("safe-shell-state", "unalias", "audit_alias",
                         prelude=("alias audit_alias='echo audit'",)),),
    "mapfile": (fixture("stdin-array", "mapfile", "-t", "JASH_AUDIT_LINES", stdin="a\nb\n"),),
    "readarray": (fixture("stdin-array", "readarray", "-t", "JASH_AUDIT_LINES", stdin="a\nb\n"),),
    "history": (fixture("history-read", "history"),),
    "fc": (fixture("history-read", "fc", "-l", expected=1, guarded=True),),
    "jobs": (fixture("private-job-list", "jobs", script="true & jobs; wait"),),
    "wait": (fixture("private-job-wait", "wait", script="true & wait"),),
    "fg": (fixture("private-job-foreground", "fg", script="sleep 0.2 & fg %1"),),
    "bg": (fixture("private-job-background", "bg", script="true & bg %1; wait"),),
    "disown": (fixture("private-job-disown", "disown", script="true & disown %1; wait"),),
    "suspend": (fixture("observed-suspend", "suspend", expected_stop=True),),
    "return": (fixture("function-return", "return 0",
                        script="audit_return() { return 0; }; audit_return"),),
    "break": (fixture("loop-break", "break",
                       script="while true; do break; done"),),
    "continue": (fixture("loop-continue", "continue",
                          script="i=0; while test $i -lt 1; do i=1; continue; done"),),
    "source": (fixture("private-source", "source", "source-fixture.jash"),),
    ".": (fixture("private-source", ".", "source-fixture.jash"),),
    "exec": (fixture("controlled-exec", "exec", "true"),),
    "eval": (fixture("controlled-eval", "eval", "true"),),
    "trap": (fixture("safe-trap", "trap", "-", "USR1"),),
    "let": (fixture("arithmetic", "let", "1+1"),),
    "shift": (fixture("positional-shift", "shift", script="set -- one; shift"),),
    "local": (fixture("function-local", "local x=a",
                       script="audit_local() { local x=a; test \"$x\" = a; }; audit_local"),),
    "readonly": (fixture("shell-declaration", "readonly", "JASH_AUDIT=value"),),
    "typeset": (fixture("shell-declaration", "typeset", "JASH_AUDIT=value"),),
    "ulimit": (fixture("resource-read", "ulimit", "-n"),),
    "umask": (fixture("private-umask", "umask", "077"),),
    "times": (fixture("resource-read", "times"),),
    "command": (fixture("controlled-command", "command", "true"),),
    "set": (fixture("shell-options", "set", "-o"),),
    "shopt": (fixture("shell-options", "shopt", "-p"),),
    "compgen": (fixture("completion-read", "compgen", "-b"),),
    "complete": (fixture("completion-read", "complete", "-p"),),
    "array": (fixture("private-array", "array", "audit_array", "one"),),
    "getopts": (fixture("positional-options", "getopts", "a", "opt",
                         script="set -- -a; getopts a opt"),),
    "abbr": (fixture("abbreviation-list", "abbr"),),
    "juxconfig": (fixture("ux-config-read", "juxconfig", expected=0),),
    "builtin": (fixture("builtin-dispatch", "builtin", "true"),),
    "enable": (fixture("builtin-list", "enable"),),
    "hash": (fixture("hash-list", "hash"),),
    "logout": (fixture("controlled-logout", "logout", expected=1),),
    "plugin": (fixture("plugin-list", "plugin", "list"),),
    "module": (fixture("module-list", "module", "list"),),
    "sigil": (fixture("private-sigil", "sigil", "off"),),
    "intent": (fixture("intent-read", "intent", "show"),),
    "calc": (fixture("calculator", "calc", "1+1"),),
    "jash_state": (fixture("state-save", "jash_state", "save"),),
    "theme": (fixture("theme-list", "theme"),),
    "statusbar": (fixture("statusbar-read", "statusbar"),),
    "spinner": (fixture("spinner-read", "spinner"),),
    "bear": (fixture("easter-egg", "bear", expected=1),),
    "kill": (fixture("self-pid-probe", "kill", "-0", "$$", expected=1,
                     script="kill -0 $$", guarded=True),),
    "ls": (fixture("private-list", "ls", "."),),
    "touch": (fixture("safe-mutation", "touch", "created-file"),),
    "mkdir": (fixture("safe-mutation", "mkdir", "created"),),
    "rmdir": (fixture("safe-mutation", "rmdir", "empty"),),
    "chmod": (fixture("safe-mutation", "chmod", "600", "mode.txt"),),
    "chown": (fixture("private-owner", "chown", str(os.getuid()), "mode.txt"),),
    "ln": (fixture("safe-mutation", "ln", "-s", "source.txt", "link"),),
    "cat": (fixture("private-read", "cat", "fixture.txt"),),
    "find": (fixture("private-read", "find", ".", "-name", "fixture.txt"),),
    "sed": (fixture("safe-mutation", "sed", "-i", "s/old/new/", "edit.txt"),),
    "wc": (fixture("private-read", "wc", "-l", "fixture.txt"),),
    "stat": (fixture("private-read", "stat", "--json", "fixture.txt"),),
    "xargs": (fixture("private-xargs", "xargs", "--", "true",
                       script="printf '%s\\n' audit | xargs -- true"),),
    "diff": (fixture("private-read", "diff", "fixture.txt", "same.txt"),),
    "awk": (fixture("private-read", "awk", "{print $1}", "fixture.txt"),),
    "rm": (fixture("safe-mutation", "rm", "remove-me"),),
    "trash": (fixture("private-trash", "trash", "status"),),
    "tread": (fixture("private-time-read", "tread", "fixture.txt"),),
    "tdiff": (fixture("private-time-diff", "tdiff", expected=1, guarded=True),),
    "grep": (fixture("private-read", "grep", "needle", "fixture.txt"),),
    "timeout": (fixture("private-timeout", "timeout", "1s", "true"),),
    "cp": (fixture("safe-mutation", "cp", "source.txt", "copy.txt"),),
    "mv": (fixture("safe-mutation", "mv", "source.txt", "moved.txt"),),
    "ps": (fixture("process-read", "ps", "--json"),),
    "proc": (fixture("private-process-snapshot", "proc", "snapshot"),),
    "du": (fixture("private-read", "du", "-s", "."),),
    "df": (fixture("private-read", "df", "--json", "."),),
    "config": (fixture("private-config-read", "config", "show"),),
    "context": (fixture("private-context", "context", "map", "."),),
    "glob": (fixture("private-glob", "glob", "--json", "*.txt"),),
    "digest": (fixture("private-digest", "digest", "--no-cache", "."),),
    "state": (fixture("private-state", "state"),),
    "json": (
        fixture("json-adapter", "json", "git", "status",
                prelude=("git init -q",)),
    ),
    "speculate": (fixture("private-speculation", "speculate", "true"),),
    "revert": (fixture("private-revert", "revert", "git", "add", expected=128,
                       script="revert git add"),),
    "cost": (fixture("private-cost", "cost", "--dry-run", "true"),),
    "contract": (fixture("private-contract", "contract", "true"),),
    "agent-guard": (fixture("guard-status", "agent-guard", "status"),),
    "adopt": (fixture("adoption-status", "adopt", "status"),),
    "agent-guide": (fixture("private-guide", "agent-guide"),),
    "brain": (fixture("brain-status", "brain", "status"),),
    "goal": (fixture("goal-recall", "goal", "recall", "audit"),),
    "llm-invoice": (fixture("invoice-status", "llm-invoice", "status"),),
    "kv": (fixture("private-kv", "kv", "list"),),
    "doctor": (fixture("private-doctor", "doctor"),),
    "recall": (fixture("private-recall", "recall", "stats", expected=1),),
    "breathe": (fixture("breathe-status", "breathe", "status"),),
    "mirror": (fixture("private-mirror", "mirror", "--history"),),
    "gossip": (fixture("gossip-channel-send", "gossip", "--channel", "audit",
                       "send", "audit-message"),),
    "sense": (fixture("risk-preview", "sense", "--risk"),),
    "forge": (fixture("forge-json", "forge", "--json", "audit scaffold"),),
    "weave": (fixture("private-weave", "weave", '{"audit":"true"}'),),
    "expect": (fixture("state-expectation", "expect", "nonempty", "fixture.txt"),),
    "explain": (fixture("blast-radius-preview", "explain", "true"),),
    "mesh": (fixture("mesh-status", "mesh"),),
    "gap": (fixture("gap-report", "gap", "audit"),),
    "dashboard": (fixture("private-dashboard", "dashboard", "--json"),),
    "checkpoint": (fixture("private-checkpoint", "checkpoint"),),
    "rewind": (fixture("dry-run", "rewind", "--dry", script="checkpoint; rewind --dry"),),
    "verify": (fixture("private-integrity", "verify", "fixture.txt"),),
    "change": (fixture("change-status", "change", "status"),),
    "mistake": (fixture("mistake-list", "mistake", "list"),),
    "token": (fixture("token-count", "token", "count"),),
    "learn": (fixture("learning-list", "learn", "list"),),
    "predict": (fixture("prediction-risk", "predict", "risk"),),
    "risk": (fixture("risk-assess", "risk", "assess"),),
    "preference": (fixture("preference-recall", "preference", "recall", "audit"),),
    "reflect": (fixture("self-reflection", "reflect"),),
    "selfcheck": (fixture("private-integrity-check", "selfcheck", "verify"),),
    "prompt": (fixture("prompt-analysis", "prompt", "optimize", "audit"),),
    "debug": (fixture("private-parse", "debug", "--parse", "true"),),
    "lint": (fixture("private-lint", "lint", "-c", "echo audit"),),
    "journal": (fixture("private-journal", "journal"),),
    "stance": (fixture("stance-list", "stance", "list"),),
    "impact": (fixture("private-impact", "impact", "fixture.txt"),),
    "handoff": (fixture("private-handoff", "handoff"),),
    "where": (fixture("jsonl-filter", "where", "kind=event", stdin='{"kind":"event"}\n'),),
    "pick": (fixture("jsonl-project", "pick", "kind", stdin='{"kind":"event"}\n'),),
    "sortby": (fixture("jsonl-sort", "sortby", "kind", stdin='{"kind":"event"}\n'),),
    "count": (fixture("jsonl-count", "count", stdin='{"kind":"event"}\n'),),
    "sum": (fixture("jsonl-sum", "sum", "value", stdin='{"value":2}\n'),),
    "avg": (fixture("jsonl-average", "avg", "value", stdin='{"value":2}\n'),),
    "uniqby": (fixture("jsonl-unique", "uniqby", "kind", stdin='{"kind":"event"}\n'),),
    "describe": (fixture("jsonl-schema", "describe", stdin='{"kind":"event"}\n'),),
    "graph": (fixture("graph-stats", "graph", "stats", script="graph build; graph stats"),),
    "mem": (fixture("memory-timeline", "mem", "timeline", expected=1,
                    script="mem distill; mem timeline"),),
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--jash", type=Path, default=Path(__file__).resolve().parents[1] / "build" / "jash")
    parser.add_argument("--json", dest="json_path", default="builtin-audit.json",
                        help="JSON report path, or - for stdout")
    parser.add_argument("--csv", dest="csv_path", default="builtin-audit.csv",
                        help="CSV report path, or - for stdout")
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT,
                        help="wall timeout per fixture in seconds (default: %(default)s)")
    parser.add_argument("--max-runs", type=int, default=None,
                        help="optional execution budget for a limited host run")
    return parser.parse_args()


def load_registry(jash: Path) -> list[dict[str, Any]]:
    env = {"PATH": SAFE_PATH, "LC_ALL": "C", "LANG": "C"}
    try:
        completed = subprocess.run(
            [str(jash), "--builtins-json"],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=env,
            text=True,
            timeout=10,
            check=False,
            start_new_session=True,
            preexec_fn=limit_child_resources,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise RuntimeError(f"cannot read builtin registry: {exc}") from exc
    if completed.returncode != 0:
        raise RuntimeError(
            f"--builtins-json failed with {completed.returncode}: {completed.stderr.strip()}"
        )
    try:
        document = json.loads(completed.stdout)
    except json.JSONDecodeError as exc:
        raise RuntimeError(f"--builtins-json is not valid JSON: {exc}") from exc
    entries = document.get("builtins") if isinstance(document, dict) else None
    if not isinstance(entries, list):
        raise RuntimeError("--builtins-json did not contain a builtins array")
    for entry in entries:
        if not isinstance(entry, dict) or not isinstance(entry.get("name"), str):
            raise RuntimeError("--builtins-json contains a malformed builtin entry")
        if not isinstance(entry.get("effect"), str):
            raise RuntimeError(f"builtin {entry['name']!r} has no declared effect")
    return entries


def minimal_env(case_root: Path, work: Path) -> dict[str, str]:
    home = case_root / "home"
    tmpdir = case_root / "tmp"
    home.mkdir(parents=True, exist_ok=True)
    tmpdir.mkdir(parents=True, exist_ok=True)
    return {
        "HOME": str(home),
        "TMPDIR": str(tmpdir),
        "PATH": SAFE_PATH,
        "PWD": str(work),
        "OLDPWD": str(work),
        "SHELL": "/bin/sh",
        "USER": "jash-audit",
        "LOGNAME": "jash-audit",
        "UID": str(os.getuid()),
        "LANG": "C",
        "LC_ALL": "C",
        "TERM": "dumb",
        # Non-interactive Jash otherwise enables the attestation chain by
        # default; the audit must not depend on a module outside the sandbox.
        "JASH_SIGIL": "0",
        "JASH_NUDGE": "0",
        "JASH_ADOPT": "0",
        "JASH_MEMO": "0",
        "JASH_LOOPGUARD": "off",
    }


def limit_child_resources() -> None:
    limits = (
        (resource.RLIMIT_CPU, 2, 3),
        (resource.RLIMIT_FSIZE, MAX_FILE_BYTES, MAX_FILE_BYTES),
        (resource.RLIMIT_CORE, 0, 0),
        (resource.RLIMIT_NOFILE, 64, 64),
        # The shared CI host can already have hundreds of processes for the
        # audit uid; keep a finite ceiling without making nested timeout/json
        # probes fail before their builtin is reached.
        (resource.RLIMIT_NPROC, 4096, 4096),
        (resource.RLIMIT_AS, 256 * 1024 * 1024, 256 * 1024 * 1024),
    )
    for kind, soft, hard in limits:
        try:
            resource.setrlimit(kind, (soft, hard))
        except (OSError, ValueError):
            # Portability: an unavailable limit must not make the audit fail
            # before the target has had a chance to run.
            continue


def snapshot_tree(root: Path) -> dict[str, tuple[Any, ...]]:
    snapshot: dict[str, tuple[Any, ...]] = {}
    if not root.exists():
        return snapshot
    for path in sorted(root.rglob("*")):
        try:
            stat = path.lstat()
        except FileNotFoundError:
            continue
        rel = str(path.relative_to(root))
        kind = "symlink" if path.is_symlink() else "dir" if path.is_dir() else "file"
        digest = ""
        if kind == "file" and stat.st_size <= MAX_FILE_BYTES:
            try:
                digest = hashlib.sha256(path.read_bytes()).hexdigest()
            except OSError:
                digest = "<unreadable>"
        target = os.readlink(path) if kind == "symlink" else ""
        snapshot[rel] = (kind, stat.st_mode & 0o7777, stat.st_size, stat.st_mtime_ns, digest, target)
    return snapshot


def mutation_diff(before: dict[str, tuple[Any, ...]], after: dict[str, tuple[Any, ...]]) -> list[dict[str, str]]:
    changes: list[dict[str, str]] = []
    for path in sorted(set(before) | set(after)):
        if path not in before:
            changes.append({"path": path, "kind": "added"})
        elif path not in after:
            changes.append({"path": path, "kind": "deleted"})
        elif before[path] != after[path]:
            changes.append({"path": path, "kind": "changed"})
    return changes


def proc_info(pid: int, scrub_root: str) -> dict[str, Any]:
    base = Path("/proc") / str(pid)
    try:
        comm = (base / "comm").read_text(errors="replace").strip()
    except OSError:
        comm = "<exited>"
    try:
        raw = (base / "cmdline").read_bytes().replace(b"\0", b" ").decode(errors="replace").strip()
    except OSError:
        raw = ""
    return {"pid": pid, "comm": comm, "cmdline": raw.replace(scrub_root, "<audit-root>")}


def descendants_of(pid: int) -> set[int]:
    seen: set[int] = set()
    pending = [pid]
    while pending:
        current = pending.pop()
        children_file = Path("/proc") / str(current) / "task" / str(current) / "children"
        try:
            children = [int(value) for value in children_file.read_text().split()]
        except (OSError, ValueError):
            continue
        for child in children:
            if child not in seen:
                seen.add(child)
                pending.append(child)
    return seen


class DescendantTracker:
    def __init__(self, pid: int, scrub_root: str) -> None:
        self.pid = pid
        self.scrub_root = scrub_root
        self.seen: dict[int, dict[str, Any]] = {}
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self._run, name="jash-audit-proc", daemon=True)

    def start(self) -> None:
        self.thread.start()

    def _run(self) -> None:
        while not self.stop.is_set():
            for child in descendants_of(self.pid):
                self.seen.setdefault(child, proc_info(child, self.scrub_root))
            self.stop.wait(0.01)

    def finish(self) -> list[dict[str, Any]]:
        self.stop.set()
        self.thread.join(timeout=1)
        for child in descendants_of(self.pid):
            self.seen.setdefault(child, proc_info(child, self.scrub_root))
        return [self.seen[pid] for pid in sorted(self.seen)]


def decode_output(data: bytes, scrub_root: str) -> str:
    text = data[:MAX_OUTPUT].decode("utf-8", errors="replace")
    if len(data) > MAX_OUTPUT:
        text += "\n<output truncated>\n"
    return text.replace(scrub_root, "<audit-root>")


def prepare_case(case_root: Path) -> Path:
    work = case_root / "work"
    work.mkdir(parents=True)
    (work / "fixture.txt").write_text("needle first\nsecond line\n", encoding="utf-8")
    (work / "same.txt").write_text("needle first\nsecond line\n", encoding="utf-8")
    (work / "different.txt").write_text("different\n", encoding="utf-8")
    (work / "source.txt").write_text("source\n", encoding="utf-8")
    (work / "remove-me").write_text("remove only inside audit sandbox\n", encoding="utf-8")
    (work / "edit.txt").write_text("old\n", encoding="utf-8")
    (work / "mode.txt").write_text("mode\n", encoding="utf-8")
    (work / "empty").mkdir()
    (work / "subdir").mkdir()
    (work / "private-dir").mkdir()
    (work / "source-fixture.jash").write_text("printf sourced\\n\n", encoding="utf-8")
    return work


def probe_script(spec: Fixture) -> str:
    command = spec.script if spec.script is not None else shlex.join(spec.argv)
    return "\n".join((*spec.prelude, command))


def process_stopped(pid: int) -> bool:
    """Return true only when the target process really entered SIGSTOP state."""
    try:
        status = (Path("/proc") / str(pid) / "status").read_text(errors="replace")
    except OSError:
        return False
    for line in status.splitlines():
        if line.startswith("State:"):
            return len(line.split()) > 1 and line.split()[1] in {"T", "t"}
    return False


def run_fixture(jash: Path, spec: Fixture, timeout: float, root: Path) -> RunResult:
    case_root = root / "case"
    case_root.mkdir(parents=True)
    work = prepare_case(case_root)
    script = probe_script(spec)
    env = minimal_env(case_root, work)
    before = snapshot_tree(case_root)
    started = time.monotonic()
    timed_out = False
    try:
        process = subprocess.Popen(
            [str(jash), "-c", script],
            cwd=work,
            env=env,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            start_new_session=True,
            preexec_fn=limit_child_resources,
        )
    except OSError as exc:
        return RunResult(spec.label, None, False, {"kind": "spawn", "message": str(exc)},
                         "", "", [], [], spec.expected_exit, False, False,
                         spec.expected_stop, False, 0.0)

    tracker = DescendantTracker(process.pid, str(case_root))
    tracker.start()
    suspended = False
    try:
        stdout, stderr = process.communicate(spec.stdin.encode(), timeout=timeout)
    except subprocess.TimeoutExpired as exc:
        timed_out = True
        suspended = spec.expected_stop and process_stopped(process.pid)
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        stdout, stderr = process.communicate()
    elapsed = time.monotonic() - started
    returncode = process.returncode
    descendants = tracker.finish()
    after = snapshot_tree(case_root)
    crash: dict[str, Any] | None = None
    if returncode is not None and returncode < 0 and not timed_out:
        number = -returncode
        crash = {"signal": signal.Signals(number).name if number in signal.Signals._value2member_map_ else number}
    passed = (
        (not timed_out and crash is None and returncode == spec.expected_exit)
        or (spec.expected_stop and suspended and returncode is not None and returncode < 0)
    ) and not spec.smoke_only
    stderr_text = decode_output(stderr, str(case_root))
    if elapsed > timeout and not timed_out:
        stderr_text += f"\n<audit wall time {elapsed:.3f}s exceeded requested timeout>\n"
        timed_out = True
        passed = False
    return RunResult(
        spec.label,
        returncode,
        timed_out,
        crash,
        decode_output(stdout, str(case_root)),
        stderr_text,
        mutation_diff(before, after),
        descendants,
        spec.expected_exit,
        passed,
        True,
        spec.expected_stop,
        suspended,
        elapsed,
    )


def case_to_dict(result: RunResult) -> dict[str, Any]:
    return {
        "label": result.label,
        "exit_code": result.exit_code,
        "expected_exit_code": result.expected_exit,
        "timeout": result.timed_out,
        "expected_timeout": result.expected_stop,
        "suspended_observed": result.suspended,
        "observed_signal": "SIGKILL" if result.suspended else (
            result.crash["signal"] if result.crash else None
        ),
        "elapsed_seconds": round(result.elapsed, 6),
        "crash": result.crash,
        "stdout": result.stdout,
        "stderr": result.stderr,
        "mutations_observed": result.mutations,
        "descendants": result.descendants,
        "status": "PASS" if result.passed else "FAIL",
        "executed": result.executed,
    }


def aggregate_record(entry: dict[str, Any], specs: tuple[Fixture, ...], results: list[RunResult]) -> dict[str, Any]:
    all_passed = bool(results) and all(result.passed for result in results)
    smoke = any(spec.smoke_only for spec in specs)
    if smoke and all_passed:
        status = "SMOKE_ONLY"
    elif all_passed and any(spec.guarded for spec in specs):
        status = "GUARDED_EXECUTED"
    elif all_passed and any(result.suspended for result in results):
        status = "OBSERVED_SUSPEND"
    elif all_passed:
        status = "PASS"
    elif any(result.timed_out for result in results):
        status = "TIMEOUT"
    elif any(result.crash for result in results):
        status = "CRASH"
    else:
        status = "FAIL"
    primary = results[0] if results else None
    mode = "FIXTURE_SAFE"
    labels = {spec.label for spec in specs}
    if "positive" in labels and "negative" in labels:
        mode = "FIXTURE_POSITIVE_NEGATIVE"
    elif labels == {"negative"}:
        mode = "FIXTURE_NEGATIVE"
    elif smoke:
        mode = "SMOKE_ONLY"
    return {
        "name": entry["name"],
        "effect_declared": entry["effect"],
        "mode": mode,
        "status": status,
        "executed": bool(results) and all(result.executed for result in results),
        "probe": [probe_script(spec) for spec in specs],
        "probe_argv": [list(spec.argv) for spec in specs],
        "expected_result": [
            {"exit_code": spec.expected_exit,
             "signal": "SIGSTOP" if spec.expected_stop else None,
             "timeout": spec.expected_stop,
             "guardrail": spec.guarded}
            for spec in specs
        ],
        "exit_code": primary.exit_code if primary else None,
        "signal": next((
            "SIGKILL" if result.suspended else result.crash["signal"]
            for result in results if result.suspended or result.crash
        ), None),
        "timeout": any(result.timed_out for result in results),
        "crash": next((result.crash for result in results if result.crash), None),
        "stdout": "\n".join(f"[{result.label}]\n{result.stdout}" for result in results),
        "stderr": "\n".join(f"[{result.label}]\n{result.stderr}" for result in results),
        "mutations_observed": [change for result in results for change in result.mutations],
        "descendants": [child for result in results for child in result.descendants],
        "cases": [case_to_dict(result) for result in results],
        "help": entry.get("help", ""),
        "argv_native": entry.get("argv_native"),
    }


def skipped_record(entry: dict[str, Any], mode: str = "UNPROBED", reason: str = "no explicit fixture") -> dict[str, Any]:
    return {
        "name": entry["name"],
        "effect_declared": entry["effect"],
        "mode": mode,
        "status": mode,
        "executed": False,
        "probe": [],
        "probe_argv": [],
        "expected_result": [],
        "exit_code": None,
        "timeout": False,
        "crash": None,
        "stdout": "",
        "stderr": "",
        "mutations_observed": [],
        "descendants": [],
        "cases": [],
        "help": entry.get("help", ""),
        "argv_native": entry.get("argv_native"),
        "skip_reason": reason,
    }


def audit(entries: list[dict[str, Any]], jash: Path, timeout: float, max_runs: int | None) -> list[dict[str, Any]]:
    records: list[dict[str, Any]] = []
    run_count = 0
    with tempfile.TemporaryDirectory(prefix="jash-builtin-audit-") as temp:
        root = Path(temp)
        for entry in entries:
            name = entry["name"]
            specs = FIXTURES.get(name)
            if not specs:
                records.append(skipped_record(entry))
                continue
            needed = len(specs)
            if max_runs is not None and run_count + needed > max_runs:
                records.append(skipped_record(entry, "UNPROBED_LIMIT", "execution budget exhausted"))
                continue
            results: list[RunResult] = []
            for index, spec in enumerate(specs):
                case_root = root / f"{len(records):04d}-{index:02d}"
                result = run_fixture(jash, spec, timeout, case_root)
                results.append(result)
                run_count += 1
            records.append(aggregate_record(entry, specs, results))
    return records


CSV_FIELDS = (
    "name", "effect_declared", "mode", "status", "executed", "probe",
    "probe_argv", "expected_result", "exit_code", "signal", "timeout", "crash",
    "stdout", "stderr",
    "mutations_observed", "descendants", "cases", "help", "argv_native",
)


def write_json(path: str, document: dict[str, Any]) -> None:
    encoded = json.dumps(document, ensure_ascii=True, indent=2) + "\n"
    if path == "-":
        sys.stdout.write(encoded)
    else:
        target = Path(path)
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(encoded, encoding="utf-8")


def write_csv(path: str, records: list[dict[str, Any]]) -> None:
    if path == "-":
        handle = sys.stdout
        close = False
    else:
        target = Path(path)
        target.parent.mkdir(parents=True, exist_ok=True)
        handle = target.open("w", newline="", encoding="utf-8")
        close = True
    try:
        writer = csv.DictWriter(handle, fieldnames=CSV_FIELDS, extrasaction="ignore")
        writer.writeheader()
        for record in records:
            row = dict(record)
            for field_name in CSV_FIELDS:
                value = row.get(field_name, "")
                if isinstance(value, (dict, list)):
                    row[field_name] = json.dumps(value, ensure_ascii=True, separators=(",", ":"))
            writer.writerow(row)
    finally:
        if close:
            handle.close()


def main() -> int:
    args = parse_args()
    jash = args.jash.resolve()
    if not jash.is_file() or not os.access(jash, os.X_OK):
        print(f"error: executable not found: {jash}", file=sys.stderr)
        return 2
    if args.timeout <= 0 or (args.max_runs is not None and args.max_runs < 0):
        print("error: timeout and max-runs must be non-negative (timeout strictly positive)", file=sys.stderr)
        return 2
    try:
        entries = load_registry(jash)
        records = audit(entries, jash, args.timeout, args.max_runs)
    except RuntimeError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    names = [entry["name"] for entry in entries]
    document = {
        "schema": "jash.builtin-audit.v2",
        "source": "build/jash --builtins-json",
        "jash": str(jash),
        "registered_count": len(entries),
        "audited_count": len(records),
        "coverage_complete": names == [record["name"] for record in records],
        "fixture_count": sum(len(record["cases"]) for record in records),
        "executed_count": sum(record.get("executed", False) for record in records),
        "guarded_count": sum(record["status"] == "GUARDED_EXECUTED" for record in records),
        "unprobed_count": sum(not record.get("executed", False) for record in records),
        "failed_count": sum(
            record["status"] in {
                "FAIL", "TIMEOUT", "CRASH", "SMOKE_ONLY", "UNPROBED", "UNPROBED_LIMIT"
            }
            or not record.get("executed", False)
            for record in records
        ),
        "records": records,
    }
    write_json(args.json_path, document)
    write_csv(args.csv_path, records)
    print(
        f"builtin audit: {len(records)}/{len(entries)} names, "
        f"{document['fixture_count']} fixture cases, "
        f"{document['executed_count']} executed, "
        f"{document['guarded_count']} guarded",
        file=sys.stderr,
    )
    return 0 if (
        document["coverage_complete"]
        and document["registered_count"] == 171
        and document["audited_count"] == 171
        and document["executed_count"] == 171
        and document["failed_count"] == 0
    ) else 1


if __name__ == "__main__":
    raise SystemExit(main())
