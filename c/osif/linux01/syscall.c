/* Author: F E R M I ∞ H A R T <contact@fermihart.com>
 * SPDX-License-Identifier: LicenseRef-SPL-1.0
 * Stable Jash OSIF request numbers -> Linux01 ILP32 traps. Structures use
 * this target's headers; unsupported modern structures/operations fail ENOSYS.
 */
#include "linux01_syscall.h"
long bear_osif_syscall6(long nr,long a,long b,long c,long d,long e,long f)
{
    (void)e;(void)f;
    switch(nr) {
    case 0:return l01_read(a,(void *)b,c);
    case 1:return l01_write(a,(const void *)b,c);
    case 2:return l01_open((const char *)a,b,c);
    case 3:return l01_close(a);
    case 8:return l01_lseek(a,b,c);
    case 12:return l01_brk((void *)a);
    case 16:return l01_ioctl(a,b,(void *)c);
    case 22:return l01_pipe((int *)a);
    case 32:return l01_dup(a);
    case 33:return l01_dup2(a,b);
    case 39:return l01_getpid();
    case 57:return l01_fork();
    case 59:return l01_execve((const char *)a,(char **)b,(char **)c);
    case 60:case 231:l01_exit(a);for(;;);
    case 61:return d?-L01_ENOSYS:l01_waitpid(a,(int *)b,c);
    case 62:return l01_kill(a,b);
    case 72:return l01_fcntl(a,b,c);
    case 80:return l01_chdir((const char *)a);
    case 83:return l01_mkdir((const char *)a,b);
    case 84:return l01_rmdir((const char *)a);
    case 86:return l01_link((const char *)a,(const char *)b);
    case 87:return l01_unlink((const char *)a);
    case 102:return l01_getuid();
    case 104:return l01_getgid();
    case 107:return l01_geteuid();
    case 109:return l01_setpgid(a,b);
    case 110:return l01_getppid();
    case 112:return l01_setsid();
    case 162:return l01_sync();
    default:return -L01_ENOSYS;
    }
}
void bear_osif_exit(int code) { l01_exit(code);for(;;); }
