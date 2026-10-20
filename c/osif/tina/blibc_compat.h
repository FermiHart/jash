/* SPDX-License-Identifier: LicenseRef-SPL-1.0 */
/* TinaLinux's blibc ABI shims, force-included only in guest builds. */
#ifndef JASH_TINA_BLIBC_COMPAT_H
#define JASH_TINA_BLIBC_COMPAT_H
#ifdef JASH_BLIBC
#include <stdlib.h>
#include "abi-linux/syscalls/syscalls.h"
#ifndef SYS_getdents64
#ifdef __NR_getdents64
#define SYS_getdents64 __NR_getdents64
#endif
#endif
#ifndef _CS_PATH
#define _CS_PATH 0
#endif
static inline double jash_tina_atof(const char *s)
{
    return bear_strtod(s, (char **)0);
}
long long bear_atoll(const char *s);
int bear_rand(void);
int bear_srand(unsigned int seed);
#ifndef atof
#define atof jash_tina_atof
#endif
#ifndef atoll
#define atoll bear_atoll
#endif
#ifndef rand
#define rand bear_rand
#endif
#ifndef srand
#define srand bear_srand
#endif
#ifndef O_ACCMODE
#define O_ACCMODE 003
#endif
#endif
#endif
