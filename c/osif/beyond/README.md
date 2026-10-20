# Jash OSIF — Beyond

The canonical Jash source lives in `../Jash`; Beyond's sovereign build consumes
this tree through `Makefile.jash JASH=../Jash JASH_OSIF=beyond
BUILD=build/jash-beyond`. This is a **qualification build**, not the default
Beyond image until the full Bear/blibc link, command probes and boot pass.

- L0: Beyond owns its boot frame, capability manifest and single syscall door
  in `surfaces/init/bshell.basm`. It translates Linux-numbered blibc calls
  through the audited `bear_linux_to_beyond` table; native calls go through
  `bear_raw_syscall6`. Do not compile the legacy `beyondbsd/syscall.c` into
  this target or define a second `bear_osif_syscall6`.
- L1: `osinfo.c` reads the versioned FS metrics from Beyond's blibc and
  exposes only capabilities with a live provider. `uname` allocates all six
  kernel fields; the OSIF exports the five documented ones. Other providers
  remain explicitly unknown until real kernel observations exist.
- `df`/`df -h`/`df -i`: report the actual mounted root tmpfs arena and inode
  slots. Other FS capacity is not guessed. `ls` uses the canonical builtin and
  Beyond's existing cwd-resolved path/syscall door.

The port is complete only after the canonical binary links under the sovereign
toolchain, boots to the real jash shell, commands execute in ring3, the local
fork's meaningful changes are carried to the canonical tree and the embedded
copy is removed. A focused Bear compile is not a boot receipt.
