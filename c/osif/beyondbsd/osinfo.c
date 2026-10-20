/* beyondbsd/osinfo.c — OSIF L1 stub (ADR-0017).
 *
 * Every provider returns OSIF_ENOSYS. The shell BOOTS in this
 * state: ps degrades to jobs-only, df says 'not supported',
 * statusbar hides the net segment. Fill providers one at a time
 * and watch `osif score` climb — this is the gradient.
 *
 * Runbook: read AGENT.md in this directory.
 * F E R M I ∞ H A R T | SPDX-License-Identifier: LicenseRef-SPL-1.0
 */
#include "../osinfo.h"
#include <string.h>

unsigned bear_osinfo_caps(void) {
    /* SYSID live via bear_uname; others pending (see AGENT.md). */
    return OSIF_CAP_SYSID;
}

const char* bear_osinfo_target(void) { return "beyondbsd"; }

int bear_osinfo_proc_get(int pid, osif_procinfo_t* out)     { (void)pid;(void)out; return OSIF_ENOSYS; }
int bear_osinfo_proc_iter(osif_proc_cb cb, void* user)      { (void)cb;(void)user; return OSIF_ENOSYS; }
int bear_osinfo_meminfo(osif_meminfo_t* out)                { (void)out;  return OSIF_ENOSYS; }
int bear_osinfo_mounts(osif_mount_cb cb, void* user)        { (void)cb;(void)user; return OSIF_ENOSYS; }
int bear_osinfo_netstat(osif_netstat_t* out)                { (void)out;  return OSIF_ENOSYS; }
int bear_osinfo_uptime(unsigned long* seconds)              { (void)seconds; return OSIF_ENOSYS; }
int bear_osinfo_loadavg(double out[3])                      { (void)out;  return OSIF_ENOSYS; }

/* ── sysid via blibc bear_uname (5×65 utsname-like buffer) ──────────────── */

extern int bear_uname(void *buf);

int bear_osinfo_sysid(osif_sysid_t* out) {
    if (!out) return -1;
    char buf[5 * 65];
    if (bear_uname(buf) < 0) return -1;
    memset(out, 0, sizeof(*out));
    strlcpy(out->sysname, buf, sizeof(out->sysname));
    strlcpy(out->nodename, buf + 65, sizeof(out->nodename));
    strlcpy(out->release, buf + 130, sizeof(out->release));
    strlcpy(out->version, buf + 195, sizeof(out->version));
    strlcpy(out->machine, buf + 260, sizeof(out->machine));
    return 0;
}
