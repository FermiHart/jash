/*═══════════════════════════════════════════════════════════════════════════════
 *
 *   osinfo.h — OSIF L1: OS information provider API (ADR-0017)
 *
 *   The second half of the OSIF contract. L0 (osif.h) carries syscalls;
 *   L1 carries STRUCTURED OS knowledge: processes, memory, mounts,
 *   network counters, system identity. Builtins and UI consume ONLY
 *   this API — never /proc paths, never sysctl, never #ifdef.
 *
 *   Every provider returns 0 on success or OSIF_ENOSYS when the target
 *   cannot supply it. Callers MUST degrade gracefully (ADR-0017 §2.3):
 *   a backend implementing only SYSID still boots a complete shell.
 *
 *   F E R M I ∞ H A R T | SPDX-License-Identifier: LicenseRef-SPL-1.0
 *═══════════════════════════════════════════════════════════════════════════════*/
#ifndef BEAR_OSINFO_H
#define BEAR_OSINFO_H

#include <stddef.h>
#include <sys/types.h>
#include <dirent.h>

#define OSIF_ENOSYS (-38)

/* ── Capability mask ─────────────────────────────────────────────────────── */

#define OSIF_CAP_SYSID   (1u << 0)
#define OSIF_CAP_PROC    (1u << 1)
#define OSIF_CAP_MEM     (1u << 2)
#define OSIF_CAP_MOUNTS  (1u << 3)
#define OSIF_CAP_NET     (1u << 4)
#define OSIF_CAP_UPTIME  (1u << 5)
#define OSIF_CAP_LOADAVG (1u << 6)
#define OSIF_CAP_SPAWN   (1u << 7)

/* Per-process observations whose zero value is also a valid measurement. */
#define OSIF_PROC_VALID_CPU     (1u << 0)
#define OSIF_PROC_VALID_RSS     (1u << 1)
#define OSIF_PROC_VALID_VSZ     (1u << 2)
#define OSIF_PROC_VALID_START   (1u << 3)
#define OSIF_PROC_VALID_THREADS (1u << 4)
#define OSIF_PROC_VALID_UID     (1u << 5)

/* What THIS backend implements (OR of OSIF_CAP_*). */
unsigned bear_osinfo_caps(void);

/* Compile-time target name ("linux", "macos", "minix", ...). */
const char* bear_osinfo_target(void);

/* ── Structures (flat, fixed-size, caller-owned — no heap) ──────────────── */

typedef struct {
    int            pid;
    int            ppid;
    int            pgrp;         /* process group, when the backend has it */
    int            uid;
    char           comm[64];
    char           state;        /* normalized Linux letters: R S D Z T I ? */
    char           tty[16];      /* real TTY name, never a Surface alias */
    char           seat[16];     /* native display seat, e.g. "seat1" */
    int            surface;      /* 0-based Surface index, -1 when unknown */
    int            surface_foreground; /* pgrp observed as a Surface foreground */
    unsigned       valid;        /* OSIF_PROC_VALID_* */
    unsigned long  rss_kb;
    unsigned long  vsz_kb;
    unsigned long  cpu_ticks;    /* utime+stime, in clock ticks */
    unsigned long  start_ticks;  /* since boot, best effort */
    int            num_threads;  /* 0 when unknown */
} osif_procinfo_t;

typedef struct {
    unsigned long total_kb;
    unsigned long free_kb;
    unsigned long available_kb;  /* == free when the OS has no better number */
} osif_meminfo_t;

typedef struct {
    char device[256];
    char mountpoint[256];
    char fstype[64];
    unsigned long long total_b;
    unsigned long long free_b;
    unsigned long long avail_b;
} osif_mount_t;

typedef struct {
    unsigned long long rx_bytes;  /* sum over physical interfaces */
    unsigned long long tx_bytes;
} osif_netstat_t;

typedef struct {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
} osif_sysid_t;

/* ── Providers ───────────────────────────────────────────────────────────── */

/* Iteration is callback-based: no dirent/procfs assumptions leak out.
 * Return non-zero from the callback to stop early. */
typedef int (*osif_proc_cb)(const osif_procinfo_t* info, void* user);
typedef int (*osif_mount_cb)(const osif_mount_t* mnt, void* user);

int bear_osinfo_proc_iter(osif_proc_cb cb, void* user);
int bear_osinfo_proc_get(int pid, osif_procinfo_t* out);
int bear_osinfo_meminfo(osif_meminfo_t* out);
int bear_osinfo_mounts(osif_mount_cb cb, void* user);
int bear_osinfo_netstat(osif_netstat_t* out);
int bear_osinfo_sysid(osif_sysid_t* out);
int bear_osinfo_uptime(unsigned long* seconds);
int bear_osinfo_loadavg(double out[3]);

/* ── Shared POSIX helpers (osif/posix_common.c) ─────────────────────────── */

/* Spawn argv[0] with args, capture stdout+stderr, enforce a timeout.
 * No /bin/sh involved — argv is execv'd directly (kills the QA
 * injection surface, works on OSes without a Bourne shell).
 * Returns 0, -1 on error, -2 on timeout. exit_code may be NULL. */
int bear_osinfo_spawn_capture(const char* const argv[],
                              const char* workdir,
                              char* out, int outsz,
                              int timeout_ms, int* exit_code);

/* d_type with stat() fallback: many filesystems (MINIX mfs, XFS, ...)
 * legally return DT_UNKNOWN. */
#define OSIF_KIND_DIR   1
#define OSIF_KIND_REG   2
#define OSIF_KIND_OTHER 0
int osif_dirent_kind(const char* dir, const struct dirent* ent);

/* Interposable renameat2 wrapper. Tests use LD_PRELOAD fault injection, so jash
 * must not export a strong renameat2 symbol that bypasses the injected libc API. */
int jash_osif_renameat2(int olddirfd, const char* oldpath,
                        int newdirfd, const char* newpath, unsigned int flags);

/* Pure-syscall recursive remove (no system("rm -rf"), no shell). */
int osif_rmtree(const char* path);

#endif /* BEAR_OSINFO_H */
