/* SPDX-License-Identifier: LicenseRef-SPL-1.0 */
/* MINIX implementation of canonical Jash's OS-information (OSIF L1) API. */
#include "../osinfo.h"

#include <string.h>
#include <sys/utsname.h>

unsigned bear_osinfo_caps(void)
{
    return OSIF_CAP_SYSID;
}

const char *bear_osinfo_target(void)
{
    return "minix";
}

int bear_osinfo_sysid(osif_sysid_t *out)
{
    struct utsname u;

    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    if (uname(&u) != 0) return -1;
    strlcpy(out->sysname, u.sysname, sizeof(out->sysname));
    strlcpy(out->nodename, u.nodename, sizeof(out->nodename));
    strlcpy(out->release, u.release, sizeof(out->release));
    strlcpy(out->version, u.version, sizeof(out->version));
    strlcpy(out->machine, u.machine, sizeof(out->machine));
    return 0;
}

int bear_osinfo_proc_iter(osif_proc_cb cb, void *user)
{
    (void)cb;
    (void)user;
    return OSIF_ENOSYS;
}

int bear_osinfo_proc_get(int pid, osif_procinfo_t *out)
{
    (void)pid;
    (void)out;
    return OSIF_ENOSYS;
}

int bear_osinfo_meminfo(osif_meminfo_t *out)
{
    (void)out;
    return OSIF_ENOSYS;
}

int bear_osinfo_mounts(osif_mount_cb cb, void *user)
{
    (void)cb;
    (void)user;
    return OSIF_ENOSYS;
}

int bear_osinfo_netstat(osif_netstat_t *out)
{
    (void)out;
    return OSIF_ENOSYS;
}

int bear_osinfo_uptime(unsigned long *seconds)
{
    (void)seconds;
    return OSIF_ENOSYS;
}

int bear_osinfo_loadavg(double out[3])
{
    (void)out;
    return OSIF_ENOSYS;
}
