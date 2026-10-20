/*═══════════════════════════════════════════════════════════════════════════════
 *
 *   osif_stat.h — struct stat timestamp field names, per target
 *
 *   POSIX names the high-resolution stat timestamps `st_atim`/`st_mtim`/
 *   `st_ctim` (struct timespec). Darwin predates that wording and ships
 *   `st_atimespec`/`st_mtimespec`/`st_ctimespec` instead, exposing the POSIX
 *   spelling only as macros under _DARWIN_C_SOURCE — which is not something
 *   core code should have to reason about at every stat() call site.
 *
 *   These macros are lvalues, so `&BEAR_ST_MTIM(st)` is a `struct timespec*`
 *   exactly like the field access it replaces.
 *
 *   NOT part of osif.h: that header is the freestanding syscall-ABI surface
 *   (stddef/stdint only, built with -ffreestanding). This one needs
 *   <sys/stat.h> and is for ordinary hosted C files.
 *
 *   F E R M I ∞ H A R T | SPDX-License-Identifier: LicenseRef-SPL-1.0
 *═══════════════════════════════════════════════════════════════════════════════*/
#ifndef BEAR_OSIF_STAT_H
#define BEAR_OSIF_STAT_H

#include <sys/stat.h>

#if defined(__APPLE__)
#  define BEAR_ST_ATIM(st) ((st).st_atimespec)
#  define BEAR_ST_MTIM(st) ((st).st_mtimespec)
#  define BEAR_ST_CTIM(st) ((st).st_ctimespec)
#else
/* Linux/glibc, BSDs, and anything else following POSIX.1-2008. */
#  define BEAR_ST_ATIM(st) ((st).st_atim)
#  define BEAR_ST_MTIM(st) ((st).st_mtim)
#  define BEAR_ST_CTIM(st) ((st).st_ctim)
#endif

#endif /* BEAR_OSIF_STAT_H */
