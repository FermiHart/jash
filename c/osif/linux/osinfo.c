/*═══════════════════════════════════════════════════════════════════════════════
 *
 *   linux/osinfo.c — OSIF L1 Linux backend
 *
 *   /proc-based providers, KEPT IN ONE FILE so the rest of the tree
 *   stays /proc-free. This is the only place in jash where procfs text
 *   formats appear. Adding a new builtin? It calls bear_osinfo_*, never
 *   open("/proc/...").
 *
 *   F E R M I ∞ H A R T | SPDX-License-Identifier: LicenseRef-SPL-1.0
 *═══════════════════════════════════════════════════════════════════════════════*/
#include "osinfo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <sys/statvfs.h>

/* Page size for vsz/rss conversions, cached. */
static long g_page_size = 0;
static long page_size(void) {
    if (!g_page_size) g_page_size = sysconf(_SC_PAGESIZE);
    return g_page_size > 0 ? g_page_size : 4096;
}

unsigned bear_osinfo_caps(void) {
    return OSIF_CAP_SYSID  | OSIF_CAP_PROC   | OSIF_CAP_MEM     |
           OSIF_CAP_MOUNTS | OSIF_CAP_NET    | OSIF_CAP_UPTIME  |
           OSIF_CAP_LOADAVG | OSIF_CAP_SPAWN;
}

const char* bear_osinfo_target(void) { return "linux"; }

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

/* ── proc_get / proc_iter ────────────────────────────────────────────────── */

static int proc_unsigned(const char* word, unsigned long* value) {
    if (!word || *word < '0' || *word > '9') return -1;
    char* end; errno = 0;
    unsigned long parsed = strtoul(word, &end, 10);
    if (errno || (*end && *end != ' ' && *end != '\n')) return -1;
    *value = parsed; return 0;
}

static int parse_stat(const char* buf, osif_procinfo_t* info) {
    const char* begin = strchr(buf, '(');
    const char* end = strrchr(buf, ')');
    if (!begin || !end || begin >= end || end[1] != ' ') { errno = EINVAL; return -1; }
    unsigned long pid;
    if (proc_unsigned(buf, &pid) || !pid || pid > INT_MAX) { errno = EINVAL; return -1; }
    size_t length = (size_t)(end - begin - 1);
    if (length >= sizeof info->comm) { errno = EOVERFLOW; return -1; }
    memcpy(info->comm, begin + 1, length); info->comm[length] = 0;
    info->pid = (int)pid;
    const char* cursor = end + 2;
    if (!*cursor || cursor[1] != ' ') { errno = EINVAL; return -1; }
    info->state = *cursor; cursor += 2;
    unsigned long user = 0, system = 0, vsize = 0, rss = 0;
    for (int field = 4; field <= 24; field++) {
        while (*cursor == ' ') cursor++;
        if (!*cursor || *cursor == '\n') { errno = EINVAL; return -1; }
        unsigned long value = 0;
        if (field == 4 || field == 5 || field == 14 || field == 15 ||
            field == 20 || field == 22 || field == 23 || field == 24) {
            if (proc_unsigned(cursor, &value)) { errno = EOVERFLOW; return -1; }
        }
        if (field == 4 || field == 5 || field == 20) {
            if (value > INT_MAX) { errno = EOVERFLOW; return -1; }
            if (field == 4) info->ppid = (int)value;
            else if (field == 5) info->pgrp = (int)value;
            else info->num_threads = (int)value;
        } else if (field == 14) user = value;
        else if (field == 15) system = value;
        else if (field == 22) info->start_ticks = value;
        else if (field == 23) vsize = value;
        else if (field == 24) rss = value;
        while (*cursor && *cursor != ' ' && *cursor != '\n') cursor++;
    }
    if (user > ULONG_MAX - system || rss > ULONG_MAX / (unsigned long)page_size()) { errno = EOVERFLOW; return -1; }
    info->cpu_ticks = user + system;
    info->vsz_kb = vsize / 1024u;
    info->rss_kb = rss * (unsigned long)page_size() / 1024u;
    info->valid = OSIF_PROC_VALID_CPU | OSIF_PROC_VALID_RSS | OSIF_PROC_VALID_VSZ |
                  OSIF_PROC_VALID_START | OSIF_PROC_VALID_THREADS;
    strlcpy(info->tty, "?", sizeof info->tty);
    /* The procfs descriptor target is a direct OS observation, unlike a
     * fabricated tty name derived from an encoded device number. */
    char path[64], target[128];
    snprintf(path, sizeof path, "/proc/%d/fd/0", info->pid);
    ssize_t n = readlink(path, target, sizeof target - 1);
    if (n > 5 && n < (ssize_t)sizeof target - 1) {
        target[n] = 0;
        if (!strncmp(target, "/dev/", 5) && strlen(target + 5) < sizeof info->tty)
            memcpy(info->tty, target + 5, strlen(target + 5) + 1);
    }
    return 0;
}

static ssize_t proc_read(const char* path, char* buf, size_t capacity) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    size_t used = 0; int error = 0;
    while (used + 1 < capacity) {
        ssize_t n = read(fd, buf + used, capacity - used - 1);
        if (n < 0) { if (errno == EINTR) continue; error = errno; break; }
        if (!n) break;
        used += (size_t)n;
    }
    if (used + 1 == capacity) error = EOVERFLOW;
    if (close(fd) && !error) error = errno;
    if (error || !used) { errno = error ? error : EIO; return -1; }
    buf[used] = 0; return (ssize_t)used;
}

static void read_status_uid(int pid, osif_procinfo_t* info) {
    char path[64], buf[8192];
    snprintf(path, sizeof path, "/proc/%d/status", pid);
    if (proc_read(path, buf, sizeof buf) < 0) return;
    const char* uid = !strncmp(buf, "Uid:", 4) ? buf : strstr(buf, "\nUid:");
    if (!uid) return;
    if (*uid == '\n') uid++;
    uid += 4;
    while (*uid == ' ' || *uid == '\t') uid++;
    char* end; errno = 0;
    unsigned long value = strtoul(uid, &end, 10);
    if (!errno && end != uid && (*end == ' ' || *end == '\t' || *end == '\n') && value <= INT_MAX) {
        info->uid = (int)value;
        info->valid |= OSIF_PROC_VALID_UID;
    }
}

int bear_osinfo_proc_get(int pid, osif_procinfo_t* out) {
    if (!out || pid <= 0) { errno = EINVAL; return -1; }
    memset(out, 0, sizeof(*out)); out->uid = -1; out->surface = -1;
    out->pid = pid;
    char path[64], buf[4096];
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    if (proc_read(path, buf, sizeof buf) < 0 || parse_stat(buf, out)) return -1;
    read_status_uid(pid, out);
    return 0;
}

static int is_numeric(const char* s) {
    if (!*s) return 0;
    while (*s) { if (*s < '0' || *s > '9') return 0; s++; }
    return 1;
}

int bear_osinfo_proc_iter(osif_proc_cb cb, void* user) {
    if (!cb) { errno = EINVAL; return -1; }
    DIR* d = opendir("/proc");
    if (!d) return -1;
    int error = 0;
    for (;;) {
        errno = 0;
        struct dirent* entry = readdir(d);
        if (!entry) { error = errno; break; }
        if (!is_numeric(entry->d_name)) continue;
        char* end; errno = 0;
        long pid = strtol(entry->d_name, &end, 10);
        if (errno || *end || pid <= 0 || pid > INT_MAX) continue;
        osif_procinfo_t info;
        if (bear_osinfo_proc_get((int)pid, &info)) {
            /* Process exit and visibility restrictions are inherent to this
             * live readable-process snapshot, not successful zero values. */
            if (errno == ENOENT || errno == ESRCH || errno == EACCES || errno == EPERM) continue;
            error = errno ? errno : EIO; break;
        }
        if (cb(&info, user)) break;
    }
    if (closedir(d) && !error) error = errno;
    if (error) { errno = error; return -1; }
    return 0;
}

/* ── meminfo ─────────────────────────────────────────────────────────────── */

int bear_osinfo_meminfo(osif_meminfo_t* out) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    FILE* f = fopen("/proc/meminfo", "r");
    if (!f) return -1;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        unsigned long v = 0;
        if      (sscanf(line, "MemTotal: %lu kB", &v) == 1)     out->total_kb     = v;
        else if (sscanf(line, "MemFree: %lu kB", &v) == 1)      out->free_kb      = v;
        else if (sscanf(line, "MemAvailable: %lu kB", &v) == 1) out->available_kb = v;
    }
    fclose(f);
    if (!out->available_kb) out->available_kb = out->free_kb;
    return 0;
}

/* ── mounts ──────────────────────────────────────────────────────────────── */

/* procfs/mtab escape whitespace and backslashes inside fields. Decode into the
 * fixed OSIF record only after checking its capacity; a shortened mountpoint
 * would make statvfs observe a different filesystem. */
static int mount_field_copy(char* dest, size_t capacity, const char* source) {
    size_t used = 0;
    while (*source) {
        unsigned char byte = (unsigned char)*source++;
        if (byte == '\\' && source[0] >= '0' && source[0] <= '3' &&
            source[1] >= '0' && source[1] <= '7' &&
            source[2] >= '0' && source[2] <= '7') {
            byte = (unsigned char)((source[0] - '0') * 64 +
                                   (source[1] - '0') * 8 + source[2] - '0');
            source += 3;
            if (!byte) { errno = EINVAL; return -1; }
        }
        if (used + 1 >= capacity) { errno = ENAMETOOLONG; return -1; }
        dest[used++] = (char)byte;
    }
    dest[used] = '\0';
    return 0;
}

static int mount_line_fields(char* line, char* fields[6]) {
    char* cursor = NULL;
    for (int i = 0; i < 6; i++) {
        fields[i] = strtok_r(i ? NULL : line, " \t\r\n", &cursor);
        if (i == 0 && (!fields[i] || fields[i][0] == '#')) return 0;
        if (!fields[i]) { errno = EINVAL; return -1; }
    }
    char* trailing = strtok_r(NULL, " \t\r\n", &cursor);
    if ((trailing && trailing[0] != '#') ||
        !is_numeric(fields[4]) || !is_numeric(fields[5])) {
        errno = EINVAL;
        return -1;
    }
    return 1;
}

int bear_osinfo_mounts(osif_mount_cb cb, void* user) {
    if (!cb) return -1;
    FILE* f = fopen("/proc/mounts", "r");
    if (!f) f = fopen("/etc/mtab", "r");
    if (!f) return -1;
    char* line = NULL;
    size_t capacity = 0;
    int failure = 0;
    int stop = 0;
    while (!stop) {
        errno = 0;
        ssize_t length = getline(&line, &capacity, f);
        if (length < 0) {
            if ((ferror(f) || !feof(f)) && !failure) failure = errno ? errno : EIO;
            break;
        }
        if (memchr(line, '\0', (size_t)length)) {
            if (!failure) failure = EINVAL;
            continue;
        }
        char* fields[6];
        int parsed = mount_line_fields(line, fields);
        if (parsed <= 0) {
            if (parsed < 0 && !failure) failure = errno;
            continue;
        }
        /* Options can be many kilobytes (notably overlay lowerdir lists).
         * getline consumes the whole record so they cannot terminate scanning
         * or turn the remainder of this row into a new mount. */
        const char* type = fields[2];
        /* Skip pseudo filesystems */
        if (strcmp(type, "proc") == 0 || strcmp(type, "sysfs") == 0 ||
            strcmp(type, "devtmpfs") == 0 || strcmp(type, "devpts") == 0 ||
            strcmp(type, "cgroup") == 0 || strcmp(type, "cgroup2") == 0 ||
            strcmp(type, "tmpfs") == 0 || strcmp(type, "pstore") == 0 ||
            strcmp(type, "debugfs") == 0 || strcmp(type, "tracefs") == 0 ||
            strcmp(type, "securityfs") == 0 || strcmp(type, "autofs") == 0 ||
            strcmp(type, "binfmt_misc") == 0 || strcmp(type, "mqueue") == 0 ||
            strcmp(type, "hugetlbfs") == 0 || strcmp(type, "nsfs") == 0 ||
            strcmp(type, "fusectl") == 0 || strcmp(type, "configfs") == 0 ||
            strcmp(type, "ramfs") == 0 || strcmp(type, "efivarfs") == 0)
            continue;

        osif_mount_t m;
        memset(&m, 0, sizeof(m));
        if (mount_field_copy(m.device, sizeof(m.device), fields[0]) != 0 ||
            mount_field_copy(m.mountpoint, sizeof(m.mountpoint), fields[1]) != 0 ||
            mount_field_copy(m.fstype, sizeof(m.fstype), fields[2]) != 0) {
            if (!failure) failure = errno;
            continue;
        }
        struct statvfs s;
        if (statvfs(m.mountpoint, &s) != 0) continue;
        if (!s.f_blocks) continue;
        if (s.f_frsize &&
            ((unsigned long long)s.f_blocks > ULLONG_MAX / s.f_frsize ||
             (unsigned long long)s.f_bfree > ULLONG_MAX / s.f_frsize ||
             (unsigned long long)s.f_bavail > ULLONG_MAX / s.f_frsize)) {
            if (!failure) failure = EOVERFLOW;
            continue;
        }
        m.total_b = (unsigned long long)s.f_blocks * s.f_frsize;
        m.free_b  = (unsigned long long)s.f_bfree  * s.f_frsize;
        m.avail_b = (unsigned long long)s.f_bavail * s.f_frsize;
        if (cb(&m, user) != 0) stop = 1;
    }
    free(line);
    if (fclose(f) != 0 && !failure) failure = errno ? errno : EIO;
    if (failure) { errno = failure; return -1; }
    return 0;
}

/* ── netstat ─────────────────────────────────────────────────────────────── */

int bear_osinfo_netstat(osif_netstat_t* out) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    FILE* f = fopen("/proc/net/dev", "r");
    if (!f) return -1;
    /* skip 2 header lines */
    char line[512];
    if (!fgets(line, sizeof(line), f) || !fgets(line, sizeof(line), f)) {
        fclose(f);
        return -1;
    }
    while (fgets(line, sizeof(line), f)) {
        char* colon = strchr(line, ':');
        if (!colon) continue;
        *colon = '\0';
        char* ifname = line;
        while (*ifname == ' ' || *ifname == '\t') ifname++;
        /* skip loopback and docker bridges */
        if (strcmp(ifname, "lo") == 0) continue;
        if (strncmp(ifname, "docker", 6) == 0) continue;
        if (strncmp(ifname, "br-", 3) == 0) continue;
        if (strncmp(ifname, "veth", 4) == 0) continue;
        unsigned long long rx, tx;
        unsigned long dummy;
        /* fields after "if:": rx_bytes rx_packets rx_errs rx_drop rx_fifo
         *                     rx_frame rx_compressed rx_multicast
         *                     tx_bytes tx_packets ... */
        if (sscanf(colon + 1,
                   "%llu %lu %lu %lu %lu %lu %lu %lu %llu",
                   &rx, &dummy, &dummy, &dummy, &dummy, &dummy, &dummy, &dummy,
                   &tx) >= 9) {
            out->rx_bytes += rx;
            out->tx_bytes += tx;
        }
    }
    fclose(f);
    return 0;
}

/* ── uptime / loadavg ────────────────────────────────────────────────────── */

int bear_osinfo_uptime(unsigned long* seconds) {
    if (!seconds) return -1;
    struct sysinfo si;
    if (sysinfo(&si) != 0) return -1;
    *seconds = (unsigned long)si.uptime;
    return 0;
}

int bear_osinfo_loadavg(double out[3]) {
    if (!out) return -1;
    FILE* f = fopen("/proc/loadavg", "r");
    if (!f) return -1;
    int n = fscanf(f, "%lf %lf %lf", &out[0], &out[1], &out[2]);
    fclose(f);
    return n == 3 ? 0 : -1;
}
