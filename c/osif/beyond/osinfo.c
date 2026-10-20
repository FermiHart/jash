/* SPDX-License-Identifier: LicenseRef-SPL-1.0 */
/* Beyond OSIF: only advertise observations provided by the live kernel.
 * Beyond is not NetBSD: its process and mount APIs have their own authority
 * boundaries, so an absent provider stays absent rather than inventing data.
 * INV-858: sys_uname writes six 65-byte fields even though OSIF exposes five.
 * INV-889: proc/mem/uptime providers live on native syscalls. */
#include "../osinfo.h"
#include <syscall/fs_metrics.h>
#include <syscall/proc_info.h>
#include <limits.h>
#include <string.h>

extern int bear_uname(void *buf);
extern int bear_fs_metrics_get(beyond_fs_metrics_v1_t *out);
extern long bear_raw_syscall6(long nr, long a0, long a1, long a2,
                               long a3, long a4, long a5);

/* Caller-owned, fresh for every query (including capability probes). Neither
 * a failed provider nor a PID from an earlier incarnation is cached. A private
 * snapshot also permits a callback to perform a nested observation safely. */
static int load_proc_table(beyond_proc_table_v2_t *table)
{
    long rc;
    memset(table, 0, sizeof(*table));
    rc = bear_raw_syscall6(0x182, (long)table, (long)sizeof(*table),
                          BEYOND_PROC_INFO_V2, 0, 0, 0);
    if (rc != 0 || table->version != BEYOND_PROC_INFO_V2 ||
        table->record_size != sizeof(beyond_proc_info_v2_t) ||
        table->count > BEYOND_PROC_MAX ||
        (table->flags & ~BEYOND_PROC_TABLE_TRUNCATED)) return 0;
    for (uint32_t i = 0; i < table->count; i++) {
        const beyond_proc_info_v2_t *p = &table->procs[i];
        if (p->pid > INT_MAX || p->ppid > INT_MAX || p->uid > INT_MAX ||
            p->reserved) return 0;
    }
    return 1;
}

static void copy_field(char *dst, size_t cap, const char *src, size_t width)
{
    size_t n = 0;
    if (!cap) return;
    while (n < width && n < cap - 1 && src[n]) n++;
    memcpy(dst, src, n);
    dst[n] = 0;
}

unsigned bear_osinfo_caps(void)
{
    beyond_fs_metrics_v1_t facts;
    beyond_proc_table_v2_t table;
    return OSIF_CAP_SYSID |
        (bear_fs_metrics_get(&facts) == 0 ? OSIF_CAP_MOUNTS : 0u) |
        (load_proc_table(&table) ? OSIF_CAP_PROC : 0u);
}
const char *bear_osinfo_target(void) { return "beyond"; }

int bear_osinfo_sysid(osif_sysid_t *out)
{
    char fields[6 * 65]; /* INV-858: kernel copyout includes domainname */
    if (!out || bear_uname(fields) < 0) return -1;
    memset(out, 0, sizeof(*out));
    copy_field(out->sysname, sizeof(out->sysname), fields, 65);
    copy_field(out->nodename, sizeof(out->nodename), fields + 65, 65);
    copy_field(out->release, sizeof(out->release), fields + 130, 65);
    copy_field(out->version, sizeof(out->version), fields + 195, 65);
    copy_field(out->machine, sizeof(out->machine), fields + 260, 65);
    return 0;
}

static char state_letter(uint32_t state)
{
    switch (state) {
    case 0: return 'I';
    case 1: return 'R';
    case 2: return 'S';
    case 3: return 'Z';
    default: return '?';
    }
}

static unsigned long bounded_ulong(uint64_t value)
{ return value > ULONG_MAX ? ULONG_MAX : (unsigned long)value; }

static void fill_proc_row(const beyond_proc_info_v2_t *p, osif_procinfo_t *row)
{
    memset(row, 0, sizeof(*row));
    row->pid = (int)p->pid;
    row->ppid = (int)p->ppid;
    row->uid = (int)p->uid;
    row->state = state_letter(p->state);
    row->surface = -1;
    row->tty[0] = '?';
    copy_field(row->comm, sizeof(row->comm), p->comm, sizeof(p->comm));
    if (p->valid_flags & BEYOND_PROC_VALID_RUNTIME) {
        row->cpu_ticks = bounded_ulong(p->runtime_us / 10000u);
        row->valid |= OSIF_PROC_VALID_CPU;
    }
    if (p->valid_flags & BEYOND_PROC_MEMORY_COHERENT) {
        if (p->valid_flags & BEYOND_PROC_VALID_RSS) {
            row->rss_kb = bounded_ulong(p->rss_bytes / 1024u);
            row->valid |= OSIF_PROC_VALID_RSS;
        }
        if (p->valid_flags & BEYOND_PROC_VALID_VSZ) {
            row->vsz_kb = bounded_ulong(p->vsz_bytes / 1024u);
            row->valid |= OSIF_PROC_VALID_VSZ;
        }
    }
    row->num_threads = (int)p->num_threads;
    if (p->num_threads && p->num_threads <= INT_MAX)
        row->valid |= OSIF_PROC_VALID_THREADS;
    else row->num_threads = 0;
}

int bear_osinfo_proc_get(int pid, osif_procinfo_t *out)
{
    beyond_proc_table_v2_t table;
    if (!out) return OSIF_ENOSYS;
    memset(out, 0, sizeof(*out));
    out->surface = -1;
    if (pid < 0 || !load_proc_table(&table)) return OSIF_ENOSYS;
    for (uint32_t i = 0; i < table.count; i++) {
        if (table.procs[i].pid <= INT_MAX && (int)table.procs[i].pid == pid) {
            fill_proc_row(&table.procs[i], out);
            return 0;
        }
    }
    return OSIF_ENOSYS;
}

int bear_osinfo_proc_iter(osif_proc_cb cb, void *user)
{
    beyond_proc_table_v2_t table;
    if (!cb || !load_proc_table(&table) ||
        (table.flags & BEYOND_PROC_TABLE_TRUNCATED)) return OSIF_ENOSYS;
    for (uint32_t i = 0; i < table.count; i++) {
        osif_procinfo_t row;
        if (table.procs[i].pid > INT_MAX || table.procs[i].ppid > INT_MAX ||
            table.procs[i].uid > INT_MAX) return OSIF_ENOSYS;
        fill_proc_row(&table.procs[i], &row);
        if (cb(&row, user)) break;
    }
    return 0;
}

int bear_osinfo_meminfo(osif_meminfo_t *out)
{ (void)out; return OSIF_ENOSYS; }
int bear_osinfo_mounts(osif_mount_cb cb, void *user)
{
    beyond_fs_metrics_v1_t facts;
    osif_mount_t mount;
    if (!cb || bear_fs_metrics_get(&facts)) return OSIF_ENOSYS;
    memset(&mount, 0, sizeof(mount));
    copy_field(mount.device, sizeof(mount.device), facts.device, sizeof(facts.device));
    copy_field(mount.mountpoint, sizeof(mount.mountpoint), facts.mountpoint, sizeof(facts.mountpoint));
    strncpy(mount.fstype, "tmpfs", sizeof(mount.fstype) - 1);
    mount.total_b = facts.total_bytes;
    mount.free_b = facts.free_bytes;
    mount.avail_b = facts.available_bytes;
    (void)cb(&mount, user);
    return 0;
}
int bear_osinfo_netstat(osif_netstat_t *out)
{ (void)out; return OSIF_ENOSYS; }
int bear_osinfo_uptime(unsigned long *seconds)
{ (void)seconds; return OSIF_ENOSYS; }
int bear_osinfo_loadavg(double out[3])
{ (void)out; return OSIF_ENOSYS; }
