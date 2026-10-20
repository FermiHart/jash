/* SPDX-License-Identifier: LicenseRef-SPL-1.0 */
/*═══════════════════════════════════════════════════════════════════════════════
 *
 *   macos/osinfo.c — OSIF L1 macOS backend
 *
 *   sysctl + libproc + Mach kernel statistics. No /proc on Darwin — the
 *   whole point of OSIF: builtins never knew. F E R M I ∞ H A R T
 *═══════════════════════════════════════════════════════════════════════════════*/
#include "osinfo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/utsname.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/proc.h>
#include <mach/mach.h>
#include <mach/mach_host.h>
#include <mach/host_info.h>
#include <mach/vm_statistics.h>
#include <libproc.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <net/route.h>
#include <ifaddrs.h>

unsigned bear_osinfo_caps(void) {
    /* Everything except CPU ticks per process (libproc reports total
     * times; mapped to cpu_ticks at usec→tick approximation). */
    return OSIF_CAP_SYSID  | OSIF_CAP_PROC   | OSIF_CAP_MEM    |
           OSIF_CAP_MOUNTS | OSIF_CAP_NET    | OSIF_CAP_UPTIME |
           OSIF_CAP_LOADAVG | OSIF_CAP_SPAWN;
}

const char* bear_osinfo_target(void) { return "macos"; }

/* ── sysid ───────────────────────────────────────────────────────────────── */

int bear_osinfo_sysid(osif_sysid_t* out) {
    if (!out) return -1;
    struct utsname u;
    if (uname(&u) != 0) return -1;
    memset(out, 0, sizeof(*out));
    strlcpy(out->sysname, u.sysname, sizeof(out->sysname));
    strlcpy(out->nodename, u.nodename, sizeof(out->nodename));
    strlcpy(out->release, u.release, sizeof(out->release));
    strlcpy(out->version, u.version, sizeof(out->version));
    strlcpy(out->machine, u.machine, sizeof(out->machine));
    return 0;
}

/* ── proc_get / proc_iter via libproc ────────────────────────────────────── */

static char map_state(int kp_state) {
    /* From sys/proc.h: SIDL=1 SRUN=2 SSLEEP=3 SSTOP=4 SZOMB=5 */
    switch (kp_state) {
        case 1: return 'I';     /* IDL */
        case 2: return 'R';
        case 3: return 'S';
        case 4: return 'T';
        case 5: return 'Z';
        default: return '?';
    }
}

int bear_osinfo_proc_get(int pid, osif_procinfo_t* out) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    out->pid = pid;
    strlcpy(out->tty, "?", sizeof(out->tty));

    struct proc_bsdinfo bi;
    int rc = proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &bi, PROC_PIDTBSDINFO_SIZE);
    if (rc <= 0) return -1;
    out->ppid = bi.pbi_ppid;
    out->uid  = (int)bi.pbi_uid;
    out->state = map_state(bi.pbi_status);
    /* pbi_comm is capped at MAXCOMLEN (16 chars); use proc_pidpath for full name */
    char pathbuf[PROC_PIDPATHINFO_MAXSIZE];
    if (proc_pidpath(pid, pathbuf, sizeof(pathbuf)) > 0) {
        const char* base = strrchr(pathbuf, '/');
        strlcpy(out->comm, base ? base + 1 : pathbuf, sizeof(out->comm));
        out->comm[sizeof(out->comm) - 1] = '\0';
    } else {
        strlcpy(out->comm, bi.pbi_comm, sizeof(out->comm));
    }
    out->start_ticks = (unsigned long)bi.pbi_start_tvsec;

    struct proc_taskinfo ti;
    rc = proc_pidinfo(pid, PROC_PIDTASKINFO, 0, &ti, PROC_PIDTASKINFO_SIZE);
    if (rc > 0) {
        out->rss_kb     = (unsigned long)(ti.pti_resident_size / 1024);
        out->vsz_kb     = (unsigned long)(ti.pti_virtual_size  / 1024);
        /* user_time + system_time are in nanoseconds; convert to "ticks"
         * at the conventional 100 Hz so cpu math in builtins is uniform
         * with the Linux backend. */
        out->cpu_ticks = (unsigned long)((ti.pti_total_user + ti.pti_total_system) / 10000000ULL);
        out->num_threads = (int)ti.pti_threadnum;
    }
    return 0;
}

int bear_osinfo_proc_iter(osif_proc_cb cb, void* user) {
    if (!cb) return -1;
    int n = proc_listallpids(NULL, 0);
    if (n <= 0) return -1;
    /* Headroom for processes that appear during enumeration. */
    int cap = n + 64;
    pid_t* pids = (pid_t*)calloc((size_t)cap, sizeof(pid_t));
    if (!pids) return -1;
    n = proc_listallpids(pids, cap * (int)sizeof(pid_t));
    if (n <= 0) { free(pids); return -1; }
    int items = n / (int)sizeof(pid_t);
    for (int i = 0; i < items; i++) {
        if (pids[i] <= 0) continue;
        osif_procinfo_t info;
        if (bear_osinfo_proc_get(pids[i], &info) == 0) {
            if (cb(&info, user) != 0) break;
        }
    }
    free(pids);
    return 0;
}

/* ── meminfo via Mach host_statistics64 ──────────────────────────────────── */

int bear_osinfo_meminfo(osif_meminfo_t* out) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));

    /* hw.memsize: total physical RAM */
    int64_t physmem = 0;
    size_t sz = sizeof(physmem);
    if (sysctlbyname("hw.memsize", &physmem, &sz, NULL, 0) == 0) {
        out->total_kb = (unsigned long)(physmem / 1024);
    }

    /* vm_statistics64 for free/available */
    mach_port_t host = mach_host_self();
    vm_statistics64_data_t vm;
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (host_statistics64(host, HOST_VM_INFO64,
                          (host_info64_t)&vm, &count) == KERN_SUCCESS) {
        vm_size_t page = 4096;
        host_page_size(host, &page);
        unsigned long long free_b  = (unsigned long long)vm.free_count    * page;
        unsigned long long inact_b = (unsigned long long)vm.inactive_count * page;
        out->free_kb      = (unsigned long)(free_b / 1024);
        out->available_kb = (unsigned long)((free_b + inact_b) / 1024);
    } else {
        out->available_kb = out->free_kb;
    }
    return 0;
}

/* ── mounts via getmntinfo ───────────────────────────────────────────────── */

int bear_osinfo_mounts(osif_mount_cb cb, void* user) {
    if (!cb) return -1;
    struct statfs* mnts = NULL;
    int n = getmntinfo(&mnts, MNT_NOWAIT);
    if (n <= 0) return -1;
    for (int i = 0; i < n; i++) {
        struct statfs* m = &mnts[i];
        if (strcmp(m->f_fstypename, "devfs")  == 0 ||
            strcmp(m->f_fstypename, "autofs") == 0 ||
            strcmp(m->f_fstypename, "nullfs") == 0 ||
            strcmp(m->f_fstypename, "fdesc")  == 0)
            continue;
        if (m->f_blocks == 0) continue;
        osif_mount_t mo;
        memset(&mo, 0, sizeof(mo));
        strlcpy(mo.device, m->f_mntfromname, sizeof(mo.device));
        strlcpy(mo.mountpoint, m->f_mntonname, sizeof(mo.mountpoint));
        strlcpy(mo.fstype, m->f_fstypename, sizeof(mo.fstype));
        mo.total_b = (unsigned long long)m->f_blocks * m->f_bsize;
        mo.free_b  = (unsigned long long)m->f_bfree  * m->f_bsize;
        mo.avail_b = (unsigned long long)m->f_bavail * m->f_bsize;
        if (cb(&mo, user) != 0) break;
    }
    return 0;
}

/* ── netstat via getifaddrs(AF_LINK) ─────────────────────────────────────── */

int bear_osinfo_netstat(osif_netstat_t* out) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    struct ifaddrs* ifap = NULL;
    if (getifaddrs(&ifap) != 0) return -1;
    for (struct ifaddrs* p = ifap; p; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_LINK) continue;
        if (!p->ifa_name) continue;
        if (strcmp(p->ifa_name, "lo0") == 0) continue;
        if (strncmp(p->ifa_name, "utun", 4) == 0) continue;
        if (strncmp(p->ifa_name, "awdl", 4) == 0) continue;
        struct if_data* d = (struct if_data*)p->ifa_data;
        if (!d) continue;
        out->rx_bytes += d->ifi_ibytes;
        out->tx_bytes += d->ifi_obytes;
    }
    freeifaddrs(ifap);
    return 0;
}

/* ── uptime via kern.boottime ────────────────────────────────────────────── */

int bear_osinfo_uptime(unsigned long* seconds) {
    if (!seconds) return -1;
    struct timeval bt;
    size_t sz = sizeof(bt);
    if (sysctlbyname("kern.boottime", &bt, &sz, NULL, 0) != 0) return -1;
    *seconds = (unsigned long)(time(NULL) - bt.tv_sec);
    return 0;
}

/* ── loadavg via getloadavg(3) ───────────────────────────────────────────── */

int bear_osinfo_loadavg(double out[3]) {
    if (!out) return -1;
    return getloadavg(out, 3) == 3 ? 0 : -1;
}
