# Jash on Linux 0.01 — canonical reduced profile

Author: F E R M I ∞ H A R T <contact@fermihart.com>

This OSIF selects thirteen unchanged canonical Jash translation units (see
`profile.mk`): editor/keymap/render, prompt/theme/writer, history, completion,
prediction, argv tokenizer, startup/version and build identity. `profile_entry.c` wires them to a ten-command
registry and Linux01 processes. Other commands run from the guest's `/bin`.

The original midnight palette is the default. Login invocations print the
existing `/etc/motd`, then the canonical short banner. The `version` command
is the untouched canonical builtin; unavailable readlink/procfs capability
returns ENOSYS, so its optional executable-path/hash fields are omitted.

`bear_port.c` supplies ILP32 brk allocation, files, directories and UTC time for
the **real portable Bear libc** memory/string/formatter sources. `syscall.c`
translates the supported stable OSIF request numbers; unknown operations fail
with `-ENOSYS`. Structures use the Linux01 target's headers.

`surface_native.c` adapts `../tina/surface.c`: the actual typed-cell, endpoint,
viewport, foreground, presentation and input-policy ioctls are retained.
The x86-64 fault-debug hook is excluded, and the Linux01 endpoint attach code
uses `/dev/surface0` + `/dev/surfin0`. `--tty` selects the legacy transport.

`predict_store.c` adds a checked ILP32 persistence envelope around the canonical
learning/scoring functions. It handles multiword commands and 14-byte Minix
filenames. The canonical text predictor loader cannot roundtrip spaces in
command names; that original file is kept byte-identical.

The Linux-Jash fork contains content-pinned snapshots and self-contained build
recipes in `scripts/jash-profile.mk`. No external checkout is needed to build
those snapshots. Refreshing is explicit via the import/snapshot scripts.

The earlier experimental replacement shell and handwritten mini-libc have
been removed: they are not part of this canonical profile.
