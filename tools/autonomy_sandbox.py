"""Build literal bubblewrap argv for the existing jash-agentd jobs supervisor.

This module never starts a process, probes a backend, or retries without isolation.
"""

from pathlib import Path
import os
import stat
import sys


POLICY = """Linux bubblewrap filesystem/process/network isolation, not a VM.
The host kernel and host administrator are outside the boundary. The selected
root-owned launcher and installed system executables/libraries are trusted.
Only /usr, /bin, /lib and /lib64 are exposed read-only (existing system aliases
are recreated); there is no unrestricted root bind, /home bind or /etc bind.
No ld.so.cache is needed or exposed. /work is the entire caller-selected staging
directory, writable for generation and read-only for checks. /tmp is private
tmpfs, /proc is private procfs, and /dev is bubblewrap's minimal device tree.
The remaining root filesystem is ephemeral, not the host root.
Mount, user, pid, network, ipc, uts and cgroup namespaces are private and required.
No host network is reachable; socket syscalls and sandbox-local networking are
not filtered. All capabilities are dropped, nested user namespaces are disabled
and asserted disabled, and bubblewrap applies no-new-privileges. A new session
prevents controlling-terminal attacks. The private PID namespace hides host
processes, prevents signalling the host supervisor, and kills detached children
when the payload (PID 1) exits. --as-pid-1 avoids exposing a bubblewrap reaper's
original launcher environment through /proc/1/environ. The supervisor must use
SIGKILL escalation as needed because namespace PID 1 has special signal semantics.
--die-with-parent also ties lifetime to the launcher/parent.
The payload environment is cleared to PATH=/usr/bin:/bin, HOME=/tmp, LANG=C;
bubblewrap additionally derives PWD=/work from --chdir.
The existing jobs supervisor must execute this argv literally, with a sanitized
launcher environment (in particular no LD_* injection), closed extra file
descriptors and non-secret stdin/stdout/stderr pipes. --clearenv cannot protect
the launcher's own dynamic loader or secrets deliberately passed as input.
The caller must keep the canonical staging path and its ancestors stable until
launch, use private staging with no external hardlinks or nested host mounts,
and exclude credentials from staging. A string argv cannot pin directory inodes;
all files, hardlinks and submounts within staging are explicitly delegated.
This is not a resource quota, syscall filter, or defense against kernel exploits
or host-side interpretation of malicious output. Resource limits, cancellation,
output handling and reaping remain the existing supervisor's responsibility.
Missing bubblewrap, unsupported mandatory flags (including --disable-userns and
--assert-userns-disabled), or unavailable namespaces must fail the job closed;
there is no unsandboxed fallback and building argv is not an availability probe.
"""


def build_argv(workspace: Path, command: list[str], *, writable: bool,
               bwrap: str = "/usr/bin/bwrap") -> list[str]:
    """Return argv, without shell parsing or execution, for a Linux sandbox.

    ValueError denotes malformed inputs (including noncanonical/symlink staging
    paths); OSError denotes a missing or untrusted launcher/system mount source.
    Unsupported platforms raise RuntimeError. The supervisor must treat any
    subsequent exec/setup failure as a failed job, never retry without bubblewrap.
    ``bwrap`` is trusted configuration, not a payload-selected executable.
    """
    if not isinstance(command, list) or not command:
        raise ValueError("command must be a nonempty list of strings")
    if any(not isinstance(arg, str) or "\0" in arg for arg in command):
        raise ValueError("command arguments must be strings without NUL")
    if not command[0] or command[0].startswith("-"):
        raise ValueError("command executable must be nonempty and not an option")
    if type(writable) is not bool:
        raise ValueError("writable must be a bool")
    if not isinstance(workspace, Path) or not workspace.is_absolute():
        raise ValueError("workspace must be an absolute pathlib.Path")
    try:
        canonical = workspace.resolve(strict=True)
        valid = (canonical == workspace and canonical != Path("/")
                 and canonical.is_dir())
    except (OSError, ValueError, RuntimeError) as exc:
        raise ValueError("workspace must be an existing canonical directory") from exc
    if not valid:
        raise ValueError("workspace must be a canonical directory, not / or a symlink")
    if not isinstance(bwrap, str) or "\0" in bwrap or not Path(bwrap).is_absolute():
        raise ValueError("bwrap must be an absolute executable path without NUL")
    if sys.platform != "linux":
        raise RuntimeError("autonomy sandbox requires Linux bubblewrap")

    executable = Path(bwrap).resolve(strict=True)
    info = executable.stat()
    if not stat.S_ISREG(info.st_mode) or not info.st_mode & 0o111:
        raise PermissionError("bwrap must be a regular executable")
    if not os.access(executable, os.X_OK):
        raise PermissionError("bwrap is not executable by this supervisor")

    argv = [str(executable), "--unshare-all", "--unshare-user", "--unshare-cgroup",
            "--disable-userns", "--assert-userns-disabled", "--die-with-parent",
            "--new-session", "--as-pid-1", "--cap-drop", "ALL", "--clearenv",
            "--setenv", "PATH", "/usr/bin:/bin", "--setenv", "HOME", "/tmp",
            "--setenv", "LANG", "C"]
    system_paths = tuple(Path(name) for name in ("/usr", "/bin", "/lib", "/lib64"))
    trusted = [executable]
    for path in system_paths:
        if path != Path("/usr") and not path.exists() and not path.is_symlink():
            continue
        source = path.resolve(strict=True)
        if not source.is_dir() or not any(
                source == allowed or allowed in source.parents for allowed in system_paths):
            raise PermissionError(f"system mount resolves outside fixed system paths: {path}")
        trusted.append(source)
        if source == path:
            argv += ["--ro-bind", str(source), str(path)]
        else:
            argv += ["--symlink", str(source), str(path)]

    # Canonical argv[0] plus protected ancestors prevents non-root replacement
    # after validation, even when the configured launcher path was an alias.
    for path in trusted:
        for component in (path, *path.parents):
            info = component.stat()
            if info.st_uid != 0 or info.st_mode & (stat.S_IWGRP | stat.S_IWOTH):
                raise PermissionError(f"untrusted root-owned system path: {component}")

    argv += ["--proc", "/proc", "--dev", "/dev", "--tmpfs", "/tmp",
             "--bind" if writable else "--ro-bind", str(canonical), "/work",
             "--chdir", "/work", "--", *command]
    return argv
