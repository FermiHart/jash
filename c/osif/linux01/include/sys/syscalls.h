/* Author: F E R M I ∞ H A R T <contact@fermihart.com>
 * SPDX-License-Identifier: LicenseRef-SPL-1.0
 * Canonical sys_* adapter. This profile reports only real Linux01 operations.
 */
#ifndef LINUX01_SYS_CALLS_H
#define LINUX01_SYS_CALLS_H
#include "port.h"
static inline const char *jash_tmpdir(void) { return "/tmp"; }
static inline long sys_read(int fd, void *p, unsigned long n) { return read(fd,p,n); }
static inline long sys_write(int fd, const void *p, unsigned long n) { return write(fd,p,n); }
static inline long sys_open(const char *p, int f, int m) { return open(p,f,m); }
static inline long sys_close(int fd) { return close(fd); }
#endif
