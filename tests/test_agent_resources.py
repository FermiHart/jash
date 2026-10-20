#!/usr/bin/env python3
"""Isolated supervisor build; never modifies the shared CMake build."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import sys

from cgroup_fixture import CgroupUnavailable, delegated_root

ROOT = Path(__file__).resolve().parents[1]


def main():
    with tempfile.TemporaryDirectory(prefix="jash-resources-") as tmp:
        binary = Path(tmp) / "resources"
        subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
            "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-O2", "-pthread", "-I", str(ROOT / "c"),
            str(ROOT / "tests/test_agent_resources.c"),
            str(ROOT / "c/core/agent_process.c"),
            str(ROOT / "c/core/execution_ir.c"),
            str(ROOT / "c/core/agent_cgroup.c"), "-Wl,--wrap=write", "-Wl,--wrap=read",
            "-Wl,--wrap=clock_gettime", "-Wl,--wrap=waitpid", "-Wl,--wrap=poll",
            "-Wl,--wrap=openat", "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary)], cwd=tmp, check=True, timeout=30)
        try:
            with delegated_root() as root:
                subprocess.run([str(binary), str(root), *sys.argv[1:]], cwd=tmp, check=True, timeout=30)
        except CgroupUnavailable as exc:
            print(f"SKIP: {exc}", file=sys.stderr)
            return 77
    return 0


if __name__ == "__main__":
    sys.exit(main())
