/* SPDX-License-Identifier: LicenseRef-SPL-1.0 */
/* BethOS OSIF L1.  The shell boots with identity information; richer
 * observations are enabled as Proc-NU and filesystem providers land. */
#include "osinfo.h"

unsigned bear_osinfo_caps(void)
{
    return OSIF_CAP_SYSID;
}

const char* bear_osinfo_target(void)
{
    return "bethos";
}

int bear_osinfo_sysid(osif_sysid_t* out)
{
    if (!out) return OSIF_ENOSYS;
    *out = (osif_sysid_t){
        "BethOS", "bethos", "0.2", "sovereign x86_64", "x86_64"
    };
    return 0;
}

int bear_osinfo_proc_iter(osif_proc_cb cb, void* user)
{
    (void)cb; (void)user; return OSIF_ENOSYS;
}

int bear_osinfo_proc_get(int pid, osif_procinfo_t* out)
{
    (void)pid; (void)out; return OSIF_ENOSYS;
}

int bear_osinfo_meminfo(osif_meminfo_t* out)
{
    (void)out; return OSIF_ENOSYS;
}

int bear_osinfo_mounts(osif_mount_cb cb, void* user)
{
    (void)cb; (void)user; return OSIF_ENOSYS;
}

int bear_osinfo_netstat(osif_netstat_t* out)
{
    (void)out; return OSIF_ENOSYS;
}

int bear_osinfo_uptime(unsigned long* seconds)
{
    (void)seconds; return OSIF_ENOSYS;
}

int bear_osinfo_loadavg(double out[3])
{
    (void)out; return OSIF_ENOSYS;
}
