/*═══════════════════════════════════════════════════════════════════════════════
 *
 *   osif.h — OS Interface header (shared across all targets)
 *
 *   The single entry point for syscalls in a bear/osif world:
 *     bear_osif_syscall6(nr, a1..a6)
 *     bear_osif_exit(code)
 *
 *   Each target (macos, netbsd, beyondbsd, linux) provides its own implementation.
 *   The core code calls ONLY these — never raw syscall(), never libc.
 *
 *   F E R M I ∞ H A R T | SPDX-License-Identifier: LicenseRef-SPL-1.0
 *═══════════════════════════════════════════════════════════════════════════════*/
#ifndef BEAR_OSIF_H
#define BEAR_OSIF_H

#include <stddef.h>
#include <stdint.h>

/* ── ABI: Linux x86_64 syscall numbers (shared reference) ───────────────── */

#define BEAR_OSIF_NR_read        0
#define BEAR_OSIF_NR_write       1
#define BEAR_OSIF_NR_open        2
#define BEAR_OSIF_NR_close       3
#define BEAR_OSIF_NR_stat        4
#define BEAR_OSIF_NR_fstat       5
#define BEAR_OSIF_NR_lstat       6
#define BEAR_OSIF_NR_poll        7
#define BEAR_OSIF_NR_lseek       8
#define BEAR_OSIF_NR_mmap        9
#define BEAR_OSIF_NR_mprotect   10
#define BEAR_OSIF_NR_munmap     11
#define BEAR_OSIF_NR_brk        12
#define BEAR_OSIF_NR_rt_sigaction 13
#define BEAR_OSIF_NR_rt_sigprocmask 14
#define BEAR_OSIF_NR_ioctl      16
#define BEAR_OSIF_NR_dup        32
#define BEAR_OSIF_NR_dup2       33
#define BEAR_OSIF_NR_nanosleep  35
#define BEAR_OSIF_NR_getpid     39
#define BEAR_OSIF_NR_socket     41
#define BEAR_OSIF_NR_connect    42
#define BEAR_OSIF_NR_accept     43
#define BEAR_OSIF_NR_sendto     44
#define BEAR_OSIF_NR_recvfrom   45
#define BEAR_OSIF_NR_sendmsg    46
#define BEAR_OSIF_NR_recvmsg    47
#define BEAR_OSIF_NR_shutdown   48
#define BEAR_OSIF_NR_bind       49
#define BEAR_OSIF_NR_listen     50
#define BEAR_OSIF_NR_clone      56
#define BEAR_OSIF_NR_fork       57
#define BEAR_OSIF_NR_vfork      58
#define BEAR_OSIF_NR_execve     59
#define BEAR_OSIF_NR_exit       60
#define BEAR_OSIF_NR_wait4      61
#define BEAR_OSIF_NR_kill       62
#define BEAR_OSIF_NR_uname      63
#define BEAR_OSIF_NR_fcntl      72
#define BEAR_OSIF_NR_fsync      74
#define BEAR_OSIF_NR_ftruncate  77
#define BEAR_OSIF_NR_getdents64 217
#define BEAR_OSIF_NR_getcwd     79
#define BEAR_OSIF_NR_chdir      80
#define BEAR_OSIF_NR_rename     82
#define BEAR_OSIF_NR_mkdir      83
#define BEAR_OSIF_NR_rmdir      84
#define BEAR_OSIF_NR_creat      85
#define BEAR_OSIF_NR_unlink     87
#define BEAR_OSIF_NR_readlink   89
#define BEAR_OSIF_NR_chmod      90
#define BEAR_OSIF_NR_getuid     102
#define BEAR_OSIF_NR_getgid     104
#define BEAR_OSIF_NR_geteuid    107
#define BEAR_OSIF_NR_getegid    108
#define BEAR_OSIF_NR_setpgid    109
#define BEAR_OSIF_NR_getppid    110
#define BEAR_OSIF_NR_setsid     112
#define BEAR_OSIF_NR_sigaltstack 132
#define BEAR_OSIF_NR_arch_prctl 158
#define BEAR_OSIF_NR_mount      165
#define BEAR_OSIF_NR_getdents   177  /* legacy */
#define BEAR_OSIF_NR_getrlimit  183
#define BEAR_OSIF_NR_getrusage  184
#define BEAR_OSIF_NR_getgroups  115
#define BEAR_OSIF_NR_setgroups  116
#define BEAR_OSIF_NR_setuid     105
#define BEAR_OSIF_NR_setgid     106
#define BEAR_OSIF_NR_setreuid   113
#define BEAR_OSIF_NR_setregid   114
#define BEAR_OSIF_NR_getresuid  120
#define BEAR_OSIF_NR_getresgid  121
#define BEAR_OSIF_NR_setresuid  117
#define BEAR_OSIF_NR_setresgid  119
#define BEAR_OSIF_NR_fchown     207
#define BEAR_OSIF_NR_lchown     208
#define BEAR_OSIF_NR_fchmod     209
#define BEAR_OSIF_NR_gettid     186
#define BEAR_OSIF_NR_tgkill     234
#define BEAR_OSIF_NR_futex      202
#define BEAR_OSIF_NR_sched_yield 24
#define BEAR_OSIF_NR_clock_gettime 228
#define BEAR_OSIF_NR_clock_getres 229
#define BEAR_OSIF_NR_mremap     25
#define BEAR_OSIF_NR_msync      26
#define BEAR_OSIF_NR_mincore    27
#define BEAR_OSIF_NR_madvise    28
#define BEAR_OSIF_NR_shmdt      226
#define BEAR_OSIF_NR_getrandom  318
#define BEAR_OSIF_NR_memfd_create 319
#define BEAR_OSIF_NR_statfs     137
#define BEAR_OSIF_NR_fstatfs    138
#define BEAR_OSIF_NR_newfstatat 262
#define BEAR_OSIF_NR_openat     257
#define BEAR_OSIF_NR_link       86
#define BEAR_OSIF_NR_symlink    88
#define BEAR_OSIF_NR_fchdir     81
#define BEAR_OSIF_NR_mknod      133
#define BEAR_OSIF_NR_chown      92
#define BEAR_OSIF_NR_pipe       22
#define BEAR_OSIF_NR_socketpair 201
#define BEAR_OSIF_NR_setsockopt 205
#define BEAR_OSIF_NR_getsockopt 206
#define BEAR_OSIF_NR_getsockname 203
#define BEAR_OSIF_NR_getpeername 204
#define BEAR_OSIF_NR_select     23
#define BEAR_OSIF_NR_mlock      149
#define BEAR_OSIF_NR_munlock    150
#define BEAR_OSIF_NR_mlockall   151
#define BEAR_OSIF_NR_munlockall 152
#define BEAR_OSIF_NR_exit_group 231
#define BEAR_OSIF_NR_getcwd_sys 79
#define BEAR_OSIF_NR_prlimit64  302
#define BEAR_OSIF_NR_getdents64_sys 217
#define BEAR_OSIF_NR_rt_sigpending 123
#define BEAR_OSIF_NR_rt_sigtimedwait 124
#define BEAR_OSIF_NR_rt_sigqueueinfo 125
#define BEAR_OSIF_NR_rt_sigsuspend 130
#define BEAR_OSIF_NR_rt_sigreturn 15
#define BEAR_OSIF_NR_setrlimit  160
#define BEAR_OSIF_NR_prctl      157
#define BEAR_OSIF_NR_sethostname 170
#define BEAR_OSIF_NR_setdomainname 171
#define BEAR_OSIF_NR_umount2    166
#define BEAR_OSIF_NR_reboot     169
#define BEAR_OSIF_NR_acct       163
#define BEAR_OSIF_NR_sync       162
#define BEAR_OSIF_NR_statx      332
#define BEAR_OSIF_NR_chroot     161
#define BEAR_OSIF_NR_epoll_create 213
#define BEAR_OSIF_NR_epoll_create1 291
#define BEAR_OSIF_NR_epoll_ctl  233
#define BEAR_OSIF_NR_epoll_wait 232
#define BEAR_OSIF_NR_epoll_pwait 281
#define BEAR_OSIF_NR_inotify_init 253
#define BEAR_OSIF_NR_inotify_add_watch 254
#define BEAR_OSIF_NR_inotify_rm_watch 255
#define BEAR_OSIF_NR_signalfd   282
#define BEAR_OSIF_NR_signalfd4  289
#define BEAR_OSIF_NR_timerfd_create 283
#define BEAR_OSIF_NR_timerfd_settime 286
#define BEAR_OSIF_NR_timerfd_gettime 287
#define BEAR_OSIF_NR_eventfd    284
#define BEAR_OSIF_NR_eventfd2   290
#define BEAR_OSIF_NR_splice     275
#define BEAR_OSIF_NR_tee        277
#define BEAR_OSIF_NR_vmsplice   278
#define BEAR_OSIF_NR_sync_file_range 274
#define BEAR_OSIF_NR_ptrace     111
#define BEAR_OSIF_NR_syslog     167
#define BEAR_OSIF_NR_getrusage_sys 98
#define BEAR_OSIF_NR_pivot_root 217
#define BEAR_OSIF_NR_semget     221
#define BEAR_OSIF_NR_semop      222
#define BEAR_OSIF_NR_semctl     224
#define BEAR_OSIF_NR_shmget     214
#define BEAR_OSIF_NR_shmat      215
#define BEAR_OSIF_NR_shmctl     216
#define BEAR_OSIF_NR_msgget     225
#define BEAR_OSIF_NR_msgsnd     227
#define BEAR_OSIF_NR_msgrcv     228
#define BEAR_OSIF_NR_msgctl     229
#define BEAR_OSIF_NR_capget     125
#define BEAR_OSIF_NR_capset     126
#define BEAR_OSIF_NR_mount_sys  165

/* Maximum NR we handle */
#define BEAR_OSIF_NR_MAX        400

/* ── ENOSYS (portable across targets) ────────────────────────────────────── */
#ifndef BEAR_OSIF_ENOSYS
#define BEAR_OSIF_ENOSYS        38
#endif

/* ── Core interface — every target MUST implement these ──────────────────── */

/**
 * bear_osif_syscall6 — single syscall entry point.
 * @nr:    Linux x86_64 syscall number
 * @a1..a6: arguments (passed in rdi, rsi, rdx, r10, r8, r9 per ABI)
 *
 * Returns: positive or zero on success, -ENOSYS (negative errno) on error.
 *
 * Target backends:
 *   macos:     translates NR → libSystem.dylib call
 *   netbsd:    translates NR → NetBSD libc call
 *   beyondbsd: forwards to blibc bear_* functions
 *   linux:     raw syscall instruction
 */
long bear_osif_syscall6(long nr,
                        long a1, long a2, long a3,
                        long a4, long a5, long a6);

/**
 * bear_osif_exit — immediate process termination.
 * Must not return.
 */
void bear_osif_exit(int code) __attribute__((noreturn));

/* ── Convenience wrappers (built on bear_osif_syscall6) ─────────────────── */

static inline long bear_osif_syscall0(long nr)
{ return bear_osif_syscall6(nr, 0, 0, 0, 0, 0, 0); }

static inline long bear_osif_syscall1(long nr, long a1)
{ return bear_osif_syscall6(nr, a1, 0, 0, 0, 0, 0); }

static inline long bear_osif_syscall2(long nr, long a1, long a2)
{ return bear_osif_syscall6(nr, a1, a2, 0, 0, 0, 0); }

static inline long bear_osif_syscall3(long nr, long a1, long a2, long a3)
{ return bear_osif_syscall6(nr, a1, a2, a3, 0, 0, 0); }

static inline long bear_osif_syscall4(long nr, long a1, long a2, long a3, long a4)
{ return bear_osif_syscall6(nr, a1, a2, a3, a4, 0, 0); }

static inline long bear_osif_syscall5(long nr, long a1, long a2, long a3, long a4, long a5)
{ return bear_osif_syscall6(nr, a1, a2, a3, a4, a5, 0); }

#endif /* BEAR_OSIF_H */
