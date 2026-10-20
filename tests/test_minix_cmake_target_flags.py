#!/usr/bin/env python3
"""Assert that CMake compiles the canonical MINIX OSIF with target macros."""

import json
import sys
from pathlib import Path

if len(sys.argv) not in (2, 3):
    raise SystemExit(
        "usage: test_minix_cmake_target_flags.py COMPILE_COMMANDS.json "
        "[MINIX_BEAR_INCLUDE_DIR]"
    )

compile_commands = Path(sys.argv[1])
entries = json.loads(compile_commands.read_text(encoding="utf-8"))
surface_include = Path(sys.argv[2]) if len(sys.argv) == 3 else None
required_sources = (
    "c/osif/ipc.c",
    "c/osif/minix/poll.c",
    "c/osif/minix/syscall.c",
    "c/osif/minix/osinfo.c",
    "c/osif/minix/surface.c",
)
errors = []
for suffix in required_sources:
    matches = [
        entry for entry in entries
        if Path(entry["file"]).as_posix().endswith("/" + suffix)
    ]
    if not matches:
        errors.append(f"CMake did not select {suffix}")
        continue
    if suffix == "c/osif/minix/surface.c" and len(matches) != 1:
        errors.append("MINIX Surface adapter must be linked only into the interactive shell")
    for entry in matches:
        command = entry.get("command", " ".join(entry.get("arguments", [])))
        for flag in ("-D__minix", "-DJASH_TARGET_MINIX=1"):
            if flag not in command:
                errors.append(f"{suffix} compile command lacks {flag}")
        if suffix == "c/osif/minix/surface.c" and surface_include:
            if f"-I{surface_include}" not in command:
                errors.append("MINIX Surface compile command lacks its narrow Bear header overlay")
            if not (surface_include / "minix_surface_api/surface.h").is_file():
                errors.append("MINIX Bear Surface ABI header was not copied to the build overlay")

if errors:
    raise SystemExit("MINIX CMake target contract failed:\n- " + "\n- ".join(errors))
print("MINIX CMake source selection and target macros PASS")
