/*═══════════════════════════════════════════════════════════════════════════════
 *
 *   osif/singuos/osinfo.c — OSIF L1 SinguOS backend (bootstrap shape)
 *
 *   SYSID only. PROC/MEM/MOUNTS/NET/UPTIME/LOADAVG return OSIF_ENOSYS until
 *   SinguOS exposes typed state surfaces (Genesis/Nerva/observer pages) —
 *   wiring those is a SinguOS-side task, not a jash stub. SPAWN is real
 *   (fork/exec through blibc on the SinguOS kernel).
 *
 *   F E R M I ∞ H A R T | SPDX-License-Identifier: LicenseRef-SPL-1.0
 *═══════════════════════════════════════════════════════════════════════════════*/
#include "../osinfo.h"

#include <stdint.h>
#include <string.h>

unsigned bear_osinfo_caps(void) {
    return OSIF_CAP_SYSID | OSIF_CAP_SPAWN | OSIF_CAP_PROC;
}

const char* bear_osinfo_target(void) { return "singuos"; }

/* sysid: use blibc's bear_uname (kernel sys_uname fills the 6×65 buffer). */
extern int bear_uname(void *buf);

int bear_osinfo_sysid(osif_sysid_t* out) {
    if (!out) return -1;
    /* Full 6×65 utsname buffer — the kernel copies all six fields. */
    char buf[6 * 65];
    if (bear_uname(buf) < 0) return -1;
    memset(out, 0, sizeof(*out));
    strncpy(out->sysname,  buf,       sizeof(out->sysname)  - 1);
    strncpy(out->nodename, buf + 65,  sizeof(out->nodename) - 1);
    strncpy(out->release,  buf + 130, sizeof(out->release)  - 1);
    strncpy(out->version,  buf + 195, sizeof(out->version)  - 1);
    strncpy(out->machine,  buf + 260, sizeof(out->machine)  - 1);
    return 0;
}

/* SinguOS kernel process enumeration: BEY_SYS_proc_list (0x164) fills a
 * bounded array of the proc-list v1 wire struct (SinguOS
 * include/singuos/proc_list_v1.h). The struct is mirrored here because the
 * jash tree must not depend on the kernel's include layout. */
#define SINGUOS_BEY_PROC_LIST 0x164L
#define SINGUOS_PROC_V1_ENTRY_SIZE 96u
#define SINGUOS_PROC_V1_NAME_BYTES 32u

struct singuos_proc_v1 {
    int32_t  pid;
    int32_t  ppid;
    int32_t  state;
    uint32_t uid;
    uint32_t gid;
    uint32_t flags;
    uint64_t cpu_ticks;
    uint64_t start_time;
    uint8_t  name[SINGUOS_PROC_V1_NAME_BYTES];
    uint8_t  reserved[24];
};
_Static_assert(sizeof(struct singuos_proc_v1) == SINGUOS_PROC_V1_ENTRY_SIZE,
               "proc_list v1 wire size drift");

extern long bear_raw_syscall6(long nr, long a0, long a1, long a2,
                              long a3, long a4, long a5);

static char singuos_state_char(int32_t state, uint32_t flags) {
    if (flags == 1u) {          /* SINGUOS_EXEC_STATE_* */
        switch (state) {
        case 2: case 3: return 'R';
        case 4: return 'S';
        case 5: case 6: return 'Z';
        case 7: case 8: return 'T';
        case 9: return 'D';
        default: return '?';
        }
    }
    switch (state) {            /* PROC_* (kernel/process.h) */
    case 2: return 'R';
    case 3: return 'S';
    case 4: return 'Z';
    default: return '?';
    }
}

static void singuos_to_osinfo(const struct singuos_proc_v1* in,
                              osif_procinfo_t* out) {
    memset(out, 0, sizeof(*out));
    out->pid = in->pid;
    out->ppid = in->ppid;
    out->uid = (int)in->uid;
    out->state = singuos_state_char(in->state, in->flags);
    strncpy(out->comm, (const char*)in->name, sizeof(out->comm) - 1);
    /* The compat surface (jash) is on the kernel console; cells have no tty. */
    if (in->flags == 1u && strcmp((const char*)in->name, "surface") == 0)
        strncpy(out->tty, "console", sizeof(out->tty) - 1);
    else
        strncpy(out->tty, "?", sizeof(out->tty) - 1);
    out->cpu_ticks = (unsigned long)in->cpu_ticks;
    out->start_ticks = (unsigned long)in->start_time;
    /* rss/vsz genuinely unknown in the v1 wire ABI: stay 0. */
}

int bear_osinfo_proc_iter(osif_proc_cb cb, void* user) {
    static struct singuos_proc_v1 entries[64];
    long n;
    if (!cb) return -1;
    n = bear_raw_syscall6(SINGUOS_BEY_PROC_LIST, (long)entries, 64L, 0, 0, 0, 0);
    if (n < 0) return OSIF_ENOSYS;
    for (long i = 0; i < n; ++i) {
        osif_procinfo_t oi;
        singuos_to_osinfo(&entries[i], &oi);
        if (cb(&oi, user)) break;
    }
    return 0;
}

int bear_osinfo_proc_get(int pid, osif_procinfo_t* out) {
    static struct singuos_proc_v1 entries[64];
    long n;
    if (!out) return -1;
    n = bear_raw_syscall6(SINGUOS_BEY_PROC_LIST, (long)entries, 64L, 0, 0, 0, 0);
    if (n < 0) return OSIF_ENOSYS;
    for (long i = 0; i < n; ++i)
        if (entries[i].pid == pid) {
            singuos_to_osinfo(&entries[i], out);
            return 0;
        }
    return OSIF_ENOSYS;
}

int bear_osinfo_meminfo(osif_meminfo_t* out)
{ (void)out; return OSIF_ENOSYS; }

int bear_osinfo_mounts(osif_mount_cb cb, void* user)
{ (void)cb; (void)user; return OSIF_ENOSYS; }

int bear_osinfo_netstat(osif_netstat_t* out)
{ (void)out; return OSIF_ENOSYS; }

int bear_osinfo_uptime(unsigned long* seconds)
{ (void)seconds; return OSIF_ENOSYS; }

int bear_osinfo_loadavg(double out[3])
{ (void)out; return OSIF_ENOSYS; }
