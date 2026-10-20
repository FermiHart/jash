/* jash_netbsd_compat.h — POSIX names jash needs that blibc exposes only as
 * bear_* (or not yet). Force-included (-include) into every jash TU on the
 * NetBSD/BeyondBSD cross build. Pure declarations; impls in the .c twin.
 *
 * F E R M I ∞ H A R T | SPDX-License-Identifier: LicenseRef-SPL-1.0 */
#ifndef JASH_NETBSD_COMPAT_H
#define JASH_NETBSD_COMPAT_H

#include <stddef.h>

/* ── Prevent blibc from redefining struct timespec ─────────────────────── */
#define BEAR_HAVE_POSIX_TIMESPEC 1
#define _STRUCT_TIMESPEC 1

/* ── Include blibc time.h so we get bear_timespec + bear_clock_gettime ─── */
#include <time.h>

/* ── clock_gettime/clock_getres: transparent cast wrapper ────────────────
 *   blibc bear_clock_gettime() takes struct bear_timespec *,
 *   but POSIX code uses struct timespec. Both have identical layout. */
#undef clock_gettime
#undef clock_getres

static inline int
jash_clock_gettime(bear_clockid_t id, struct timespec *tp)
{
    return bear_clock_gettime(id, (struct bear_timespec *)tp);
}
#define clock_gettime(id, tp) jash_clock_gettime((id), (tp))

static inline int
jash_clock_getres(bear_clockid_t id, struct timespec *tp)
{
    return bear_clock_getres(id, (struct bear_timespec *)tp);
}
#define clock_getres(id, tp) jash_clock_getres((id), (tp))

/* ── Missing POSIX functions ───────────────────────────────────────────── */

int  nanosleep(const struct timespec *req, struct timespec *rem); /* -> bear_nanosleep */
int  setpgid(int pid, int pgid);                /* -> bear_setpgid   */
int  getpgrp(void);                             /* -> bear_getpgrp   */
int  mkstemp(char *tmpl);                        /* sovereign impl    */
int  sscanf(const char *str, const char *fmt, ...); /* minimal impl   */
int  sched_yield(void);                          /* -> bear_sched_yield */
int  usleep(unsigned usec);                      /* -> bear_nanosleep */
int  getuid(void);                               /* -> bear_getuid    */
/* INV-BEMU-PORT-UID-001: match blibc/POSIX uid_t's unsigned return. The
 * canonical unistd declaration must coexist with this older compat header. */
unsigned int geteuid(void);                      /* -> bear_geteuid   */
int  getgid(void);                               /* -> bear_getgid    */

#endif
