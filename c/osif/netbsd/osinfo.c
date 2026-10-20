/*═══════════════════════════════════════════════════════════════════════════════
 *
 *   netbsd/osinfo.c — OSIF L1 NetBSD backend
 *
 *   sysctl(KERN_PROC2) + sysctlbyname + getmntinfo + getifaddrs. No /proc
 *   dependency (NetBSD procfs is optional), no -lkvm (plain sysctl only).
 *   Copied from c/osif/macos/osinfo.c (BSD shape) and adapted to NetBSD:
 *   - proc via CTL_KERN/KERN_PROC2/KERN_PROC_ALL (struct kinfo_proc2)
 *   - mem via hw.physmem64 + vm.uvmexp2 (struct uvmexp_sysctl)
 *   - mounts via getmntinfo(struct statvfs)
 *   - net via getifaddrs(AF_LINK) + struct if_data
 *   - uptime via kern.boottime, loadavg via getloadavg(3)
 *
 *   Every provider degrades to OSIF_ENOSYS-grade -1; the shell boots with
 *   SYSID alone (ADR-0017 §2.3).
 *
 *   F E R M I ∞ H A R T | SPDX-License-Identifier: LicenseRef-SPL-1.0
 *═══════════════════════════════════════════════════════════════════════════════*/
#include "osinfo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/utsname.h>
#include <sys/mount.h>
#include <sys/statvfs.h>
#include <sys/param.h>
#include <uvm/uvm_extern.h>
#include <net/if.h>
#include <ifaddrs.h>

unsigned bear_osinfo_caps(void) {
    return OSIF_CAP_SYSID  | OSIF_CAP_PROC   | OSIF_CAP_MEM     |
           OSIF_CAP_MOUNTS | OSIF_CAP_NET    | OSIF_CAP_UPTIME  |
           OSIF_CAP_LOADAVG | OSIF_CAP_SPAWN;
}

const char* bear_osinfo_target(void) { return "netbsd"; }

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

/* ── proc via sysctl KERN_PROC2 (no -lkvm) ──────────────────────────────── */

static char map_state(int st) {
    /* sys/proc.h: SIDL=1 SRUN=2 SSLEEP=3 SSTOP=4 SZOMB=5 (SDEAD=6, SDYING=7) */
    switch (st) {
        case 1: return 'I';
        case 2: return 'R';
        case 3: return 'S';
        case 4: return 'T';
        case 5: return 'Z';
        default: return '?';
    }
}

static long g_page_kb = 0;
static long page_kb(void) {
    if (!g_page_kb) {
        long ps = sysconf(_SC_PAGESIZE);
        g_page_kb = ps > 0 ? ps / 1024 : 4;
    }
    return g_page_kb;
}

static void kinfo_to_info(const struct kinfo_proc2 *kp, osif_procinfo_t *out) {
    memset(out, 0, sizeof(*out));
    out->pid   = kp->p_pid;
    out->ppid  = kp->p_ppid;
    out->uid   = (int)kp->p_uid;
    out->state = map_state(kp->p_stat);
    strlcpy(out->tty, "?", sizeof(out->tty));
    strlcpy(out->comm, kp->p_comm, sizeof(out->comm));
    out->rss_kb     = (unsigned long)kp->p_vm_rssize * (unsigned long)page_kb();
    out->vsz_kb     = (unsigned long)kp->p_vm_vsize * (unsigned long)page_kb();
    out->cpu_ticks  = (unsigned long)(kp->p_uticks + kp->p_sticks);
    out->start_ticks = (unsigned long)kp->p_ustart_sec;
    out->num_threads = (int)kp->p_nlwps;
}

int bear_osinfo_proc_get(int pid, osif_procinfo_t* out) {
    if (!out || pid <= 0) return -1;
    int mib[6] = { CTL_KERN, KERN_PROC2, KERN_PROC_PID, (int)pid,
                   (int)sizeof(struct kinfo_proc2), 1 };
    size_t len = 0;
    /* Two-step: probe first (oldp=NULL). NOTE: the kernel adds
     * KERN_PROCSLOP to the probe result (kern_proc.c), so len is NOT the
     * exact struct size — never compare it against sizeof(kp). */
    if (sysctl(mib, 6, NULL, &len, NULL, 0) != 0 || len == 0) return -1;
    struct kinfo_proc2 kp;
    memset(&kp, 0, sizeof(kp));
    len = sizeof(kp);
    if (sysctl(mib, 6, &kp, &len, NULL, 0) != 0) return -1;
    if (len < sizeof(kp) || kp.p_pid != pid) return -1;
    kinfo_to_info(&kp, out);
    return 0;
}

int bear_osinfo_proc_iter(osif_proc_cb cb, void* user) {
    if (!cb) return -1;
    int mib[6] = { CTL_KERN, KERN_PROC2, KERN_PROC_ALL, 0,
                   (int)sizeof(struct kinfo_proc2), 0 };
    size_t len = 0;
    if (sysctl(mib, 6, NULL, &len, NULL, 0) != 0) return -1;
    if (len == 0) return -1;
    size_t n = len / sizeof(struct kinfo_proc2);
    if (n == 0) return -1;
    /* mib[5] is the STRUCT COUNT and the kernel returns ENOMEM when procs
     * appear between probe and read (needed > copied, kern_proc.c) — set a
     * GENEROUS count matching the buffer headroom, not just n. */
    mib[5] = (int)(n + 64);
    size_t cap = len + 64 * sizeof(struct kinfo_proc2);
    struct kinfo_proc2 *buf = (struct kinfo_proc2 *)malloc(cap);
    if (!buf) return -1;
    len = cap;
    if (sysctl(mib, 6, buf, &len, NULL, 0) != 0) { free(buf); return -1; }
    n = len / sizeof(struct kinfo_proc2);
    for (size_t i = 0; i < n; i++) {
        if (buf[i].p_pid <= 0) continue;
        osif_procinfo_t info;
        kinfo_to_info(&buf[i], &info);
        if (cb(&info, user) != 0) break;
    }
    free(buf);
    return 0;
}

/* ── meminfo via hw.physmem64 + vm.uvmexp2 ──────────────────────────────── */

int bear_osinfo_meminfo(osif_meminfo_t* out) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));

    uint64_t phys = 0;
    size_t sz = sizeof(phys);
    if (sysctlbyname("hw.physmem64", &phys, &sz, NULL, 0) == 0 && phys)
        out->total_kb = (unsigned long)(phys / 1024);
    else {
        unsigned long pages = 0, pgsz = 0;
        sz = sizeof(pages);
        if (sysctlbyname("hw.physmem", &pages, &sz, NULL, 0) == 0) {
            sz = sizeof(pgsz);
            pgsz = (unsigned long)sysconf(_SC_PAGESIZE);
            out->total_kb = (unsigned long)((pages * pgsz) / 1024);
        }
    }

    struct uvmexp_sysctl uvm;
    sz = sizeof(uvm);
    if (sysctlbyname("vm.uvmexp2", &uvm, &sz, NULL, 0) == 0 && uvm.pagesize > 0) {
        unsigned long long free_b =
            (unsigned long long)uvm.free * (unsigned long long)uvm.pagesize;
        out->free_kb = (unsigned long)(free_b / 1024);
        out->available_kb = out->free_kb;
    } else {
        out->available_kb = out->free_kb;
    }
    return 0;
}

/* ── mounts via getmntinfo(struct statvfs) ──────────────────────────────── */

int bear_osinfo_mounts(osif_mount_cb cb, void* user) {
    if (!cb) return -1;
    struct statvfs *mnts = NULL;
    int n = getmntinfo(&mnts, MNT_NOWAIT);
    if (n <= 0) return -1;
    for (int i = 0; i < n; i++) {
        struct statvfs *m = &mnts[i];
        if (strcmp(m->f_fstypename, "procfs") == 0 ||
            strcmp(m->f_fstypename, "kernfs") == 0 ||
            strcmp(m->f_fstypename, "ptyfs") == 0 ||
            strcmp(m->f_fstypename, "tmpfs") == 0 ||
            strcmp(m->f_fstypename, "null") == 0)
            continue;
        if (m->f_blocks == 0) continue;
        osif_mount_t mo;
        memset(&mo, 0, sizeof(mo));
        strlcpy(mo.device, m->f_mntfromname, sizeof(mo.device));
        strlcpy(mo.mountpoint, m->f_mntonname, sizeof(mo.mountpoint));
        strlcpy(mo.fstype, m->f_fstypename, sizeof(mo.fstype));
        mo.total_b = (unsigned long long)m->f_blocks * m->f_frsize;
        mo.free_b  = (unsigned long long)m->f_bfree  * m->f_frsize;
        mo.avail_b = (unsigned long long)m->f_bavail * m->f_frsize;
        if (cb(&mo, user) != 0) break;
    }
    return 0;
}

/* ── netstat via getifaddrs(AF_LINK) ─────────────────────────────────────── */

int bear_osinfo_netstat(osif_netstat_t* out) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    struct ifaddrs *ifap = NULL;
    if (getifaddrs(&ifap) != 0) return -1;
    for (struct ifaddrs *p = ifap; p; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_LINK) continue;
        if (!p->ifa_name) continue;
        if (strcmp(p->ifa_name, "lo0") == 0) continue;
        struct if_data *d = (struct if_data *)p->ifa_data;
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
