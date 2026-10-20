/* SPDX-License-Identifier: LicenseRef-SPL-1.0 */
/* TinaLinux OSIF L1: typed Proc-NU observations, Linux syscall ABI at L0.
 * Missing fields stay unknown rather than being inferred from textual procfs.
 * The Proc-NU SDK is supplied by TinaLinux at configure time. */
#include "osinfo.h"
#include "tina_procnu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <linux/tina_surface.h>
#include <sys/ioctl.h>

#define PROC_CAP (512u * 1024u)
#define AGG_CAP  (64u * 1024u)

unsigned bear_osinfo_caps(void) {
    return OSIF_CAP_SYSID | OSIF_CAP_PROC | OSIF_CAP_MEM |
           OSIF_CAP_MOUNTS | OSIF_CAP_NET | OSIF_CAP_UPTIME |
           OSIF_CAP_LOADAVG | OSIF_CAP_SPAWN;
}
const char* bear_osinfo_target(void) { return "tina"; }

int bear_osinfo_sysid(osif_sysid_t* out) {
    struct utsname u;
    if (!out || uname(&u)) return -1;
    memset(out, 0, sizeof(*out));
    strncpy(out->sysname, u.sysname, sizeof(out->sysname) - 1);
    strncpy(out->nodename, u.nodename, sizeof(out->nodename) - 1);
    strncpy(out->release, u.release, sizeof(out->release) - 1);
    strncpy(out->version, u.version, sizeof(out->version) - 1);
    strncpy(out->machine, u.machine, sizeof(out->machine) - 1);
    return 0;
}

static unsigned char* nu_slurp(const char* path, size_t cap, size_t* len) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return NULL;
    unsigned char* data = malloc(cap);
    if (!data) { close(fd); return NULL; }
    int rc = tpn_slurp_cap(fd, data, cap, len);
    close(fd);
    if (rc != TPN_OK) { free(data); return NULL; }
    return data;
}

static char state_letter(unsigned long long state) {
    switch (state) {
    case 0: return 'R';
    case 1: return 'S';
    case 2: return 'T';
    case 3: return 'Z';
    case 4: return 'X';
    default: return '?';
    }
}

/* Match tina-ps: Surface association is an observation of pgrp against the
 * foreground group published by each native Surface, never a TTY heuristic. */
static int g_surface_pgrp[SURFACE_MAX];

static void load_surface_bindings(void) {
    for (int idx = 0; idx < SURFACE_MAX; idx++) {
        char path[40];
        struct surf_foreground_t fg = {0};
        int fd;

        g_surface_pgrp[idx] = 0;
        snprintf(path, sizeof(path), "/dev/tina-surface%d", idx);
        fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;
        if (ioctl(fd, SURF_GET_FOREGROUND, &fg) == 0 && fg.pgid > 0)
            g_surface_pgrp[idx] = fg.pgid;
        close(fd);
    }
}

static void resolve_surface(int pgrp, osif_procinfo_t* row) {
    int found = -1;
    int matches = 0;

    row->surface = -1;
    row->surface_foreground = 0;
    strncpy(row->seat, "-", sizeof(row->seat) - 1);
    if (pgrp <= 0) return;
    for (int idx = 0; idx < SURFACE_MAX; idx++) {
        if (g_surface_pgrp[idx] == pgrp) {
            found = idx;
            matches++;
        }
    }
    if (matches == 0) return;
    row->surface_foreground = 1;
    if (matches > 1) {
        strncpy(row->seat, "multi", sizeof(row->seat) - 1);
        return;
    }
    row->surface = found;
    snprintf(row->seat, sizeof(row->seat), "seat%u", surf_seat_name((unsigned)found));
}

static void process_row(const tpn_process_view* v, osif_procinfo_t* row) {
    char proc_path[32];
    struct stat proc_stat;
    memset(row, 0, sizeof(*row));
    row->surface = -1;
    row->pid = (int)v->pid;
    row->ppid = (int)v->ppid;
    row->pgrp = (int)v->pgrp;
    row->uid = (uid_t)-1; /* Preserve unknown if the process exited meanwhile. */
    snprintf(proc_path, sizeof(proc_path), "/proc/%d", row->pid);
    if (stat(proc_path, &proc_stat) == 0)
        row->uid = proc_stat.st_uid;
    size_t n = v->comm_len;
    if (n >= sizeof(row->comm)) n = sizeof(row->comm) - 1;
    memcpy(row->comm, v->comm, n);
    row->comm[n] = 0;
    row->state = state_letter(v->state);
    /* Tina has native Surfaces, not TTYs. Keep the compatibility field
     * explicit rather than relabeling a Surface as a terminal device. */
    strncpy(row->tty, "-", sizeof(row->tty) - 1);
    if (v->valid & TPN_PROCESS_VALID_RSS) {
        row->rss_kb = (unsigned long)(v->rss_bytes / 1024u);
        row->valid |= OSIF_PROC_VALID_RSS;
    }
    if (v->valid & TPN_PROCESS_VALID_CPU) {
        long hz = sysconf(_SC_CLK_TCK);
        if (hz <= 0 || hz > 1000000000L) hz = 100;
        row->cpu_ticks = (unsigned long)((v->cpu_user_ns + v->cpu_system_ns) /
                                         (1000000000ull / (unsigned long long)hz));
        row->valid |= OSIF_PROC_VALID_CPU;
    }
    resolve_surface(row->pgrp, row);
}

static int process_batch(tpn_process_iter* it, unsigned char** data) {
    size_t len = 0;
    *data = nu_slurp("/proc/.nu/processes", PROC_CAP, &len);
    if (!*data) return -1;
    if (tpn_processes_open(*data, len, it) == TPN_OK) return 0;
    free(*data);
    *data = NULL;
    return -1;
}

int bear_osinfo_proc_get(int pid, osif_procinfo_t* out) {
    tpn_process_iter it;
    tpn_process_view v;
    unsigned char* data = NULL;
    int found = -1;
    if (!out || pid <= 0 || process_batch(&it, &data)) return -1;
    load_surface_bindings();
    while (tpn_processes_next(&it, &v) == TPN_OK) {
        if (v.pid == (unsigned long long)pid) {
            process_row(&v, out);
            found = 0;
            break;
        }
    }
    free(data);
    return found;
}

int bear_osinfo_proc_iter(osif_proc_cb cb, void* user) {
    tpn_process_iter it;
    tpn_process_view v;
    unsigned char* data = NULL;
    if (!cb || process_batch(&it, &data)) return -1;
    load_surface_bindings();
    while (tpn_processes_next(&it, &v) == TPN_OK) {
        osif_procinfo_t row;
        process_row(&v, &row);
        if (cb(&row, user)) break;
    }
    free(data);
    return 0;
}

int bear_osinfo_meminfo(osif_meminfo_t* out) {
    size_t len = 0;
    if (!out) return -1;
    unsigned char* data = nu_slurp("/proc/.nu/mem", AGG_CAP, &len);
    if (!data) return -1;
    tpn_mem_view view;
    int rc = tpn_parse_mem(data, len, &view);
    free(data);
    if (rc != TPN_OK || !view.has_total || !view.has_avail ||
        view.available > view.total) return -1;
    memset(out, 0, sizeof(*out));
    out->total_kb = (unsigned long)(view.total / 1024u);
    out->free_kb = (unsigned long)(view.free / 1024u);
    out->available_kb = (unsigned long)(view.available / 1024u);
    return 0;
}

/* Filesystem enumeration has no typed Proc-NU counterpart yet. */
int bear_osinfo_mounts(osif_mount_cb cb, void* user) {
    if (!cb) return -1;
    FILE* f = fopen("/proc/mounts", "r");
    if (!f) f = fopen("/etc/mtab", "r");
    if (!f) return -1;
    char dev[256], mp[256], type[64], opts[256];
    int dump, pass;
    while (fscanf(f, "%255s %255s %63s %255s %d %d",
                  dev, mp, type, opts, &dump, &pass) == 6) {
        if (!strcmp(type, "proc") || !strcmp(type, "sysfs") ||
            !strcmp(type, "devtmpfs") || !strcmp(type, "devpts") ||
            !strcmp(type, "cgroup") || !strcmp(type, "cgroup2") ||
            !strcmp(type, "tmpfs") || !strcmp(type, "pstore") ||
            !strcmp(type, "debugfs") || !strcmp(type, "tracefs") ||
            !strcmp(type, "securityfs") || !strcmp(type, "autofs") ||
            !strcmp(type, "binfmt_misc") || !strcmp(type, "mqueue") ||
            !strcmp(type, "hugetlbfs") || !strcmp(type, "nsfs") ||
            !strcmp(type, "fusectl") || !strcmp(type, "configfs") ||
            !strcmp(type, "ramfs") || !strcmp(type, "efivarfs")) continue;
        struct statvfs s;
        if (statvfs(mp, &s)) continue;
        /* An initramfs root has no block count. Its identity is still real;
         * report size 0 (unknown), not a fabricated capacity or no root. */
        if (!s.f_blocks && strcmp(mp, "/")) continue;
        osif_mount_t m;
        memset(&m, 0, sizeof(m));
        strncpy(m.device, dev, sizeof(m.device) - 1);
        strncpy(m.mountpoint, mp, sizeof(m.mountpoint) - 1);
        strncpy(m.fstype, type, sizeof(m.fstype) - 1);
        m.total_b = (unsigned long long)s.f_blocks * s.f_frsize;
        m.free_b = (unsigned long long)s.f_bfree * s.f_frsize;
        m.avail_b = (unsigned long long)s.f_bavail * s.f_frsize;
        if (cb(&m, user)) break;
    }
    fclose(f);
    return 0;
}

int bear_osinfo_netstat(osif_netstat_t* out) {
    size_t len = 0;
    if (!out) return -1;
    unsigned char* data = nu_slurp("/proc/.nu/net", AGG_CAP, &len);
    if (!data) return -1;
    tpn_net_iter it;
    int rc = tpn_net_open(data, len, &it);
    if (rc == TPN_OK) {
        memset(out, 0, sizeof(*out));
        tpn_net_iface_view row;
        while (it.remaining && tpn_net_next(&it, &row) == TPN_OK) {
            if (!row.loopback && row.up) {
                out->rx_bytes += row.rx_bytes;
                out->tx_bytes += row.tx_bytes;
            }
        }
    }
    free(data);
    return rc == TPN_OK ? 0 : -1;
}

int bear_osinfo_uptime(unsigned long* seconds) {
    size_t len = 0;
    if (!seconds) return -1;
    unsigned char* data = nu_slurp("/proc/.nu/uptime", AGG_CAP, &len);
    if (!data) return -1;
    tpn_uptime_view view;
    int rc = tpn_parse_uptime(data, len, &view);
    free(data);
    if (rc != TPN_OK || !view.has_uptime) return -1;
    *seconds = (unsigned long)(view.uptime_ns / 1000000000ull);
    return 0;
}

int bear_osinfo_loadavg(double out[3]) {
    size_t len = 0;
    if (!out) return -1;
    unsigned char* data = nu_slurp("/proc/.nu/loadavg", AGG_CAP, &len);
    if (!data) return -1;
    tpn_load_view view;
    int rc = tpn_parse_load(data, len, &view);
    free(data);
    if (rc != TPN_OK || !view.has_avg) return -1;
    out[0] = (double)view.avg_1m / 1000.0;
    out[1] = (double)view.avg_5m / 1000.0;
    out[2] = (double)view.avg_15m / 1000.0;
    return 0;
}
