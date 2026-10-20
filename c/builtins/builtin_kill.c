// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// 🎵 Hear F E R M I ∞ H A R T music. No obligation. Just a wish.
//
// Module: builtins/kill — disruptive kill builtin with name-based
// killing, batch operations, safety checks, and dry-run mode.
//
// Features:
//   kill <pid>              — traditional PID-based kill (SIGTERM)
//   kill -9 <pid>           — send SIGKILL to PID
//   kill <name>             — kill all processes matching <name>
//   kill <pattern>          — kill all processes whose comm matches pattern
//   kill --dry-run <target> — show what would be killed without sending signals
//   kill --safe <target>    — block critical system processes from being killed
//   kill -l                 — list signal names
//
// Reads /proc/[pid]/comm for process name matching on Linux.
// Falls back to error on non-Linux systems for name-based killing.

#include "builtin.h"
#include "discovery_output.h"
#include <core/query_cancel.h>
#include <stdlib.h>
#include <unistd.h>
#include "osif/osinfo.h"
#include <sys/syscalls.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <dirent.h>
#include <ctype.h>
#include <signal.h>
#include <errno.h>
#include <limits.h>

/* ── Signal name table ────────────────────────────────────────────────────── */

typedef struct {
    int         num;
    const char* name;
} kill_signal_entry_t;

static const kill_signal_entry_t KILL_SIGNALS[] = {
    { SIGHUP,    "HUP"    },
    { SIGINT,    "INT"    },
    { SIGQUIT,   "QUIT"   },
    { SIGILL,    "ILL"    },
    { SIGTRAP,   "TRAP"   },
    { SIGABRT,   "ABRT"   },
    { SIGBUS,    "BUS"    },
    { SIGFPE,    "FPE"    },
    { SIGKILL,   "KILL"   },
    { SIGUSR1,   "USR1"   },
    { SIGSEGV,   "SEGV"   },
    { SIGUSR2,   "USR2"   },
    { SIGPIPE,   "PIPE"   },
    { SIGALRM,   "ALRM"   },
    { SIGTERM,   "TERM"   },
    { SIGCHLD,   "CHLD"   },
    { SIGCONT,   "CONT"   },
    { SIGSTOP,   "STOP"   },
    { SIGTSTP,   "TSTP"   },
    { SIGTTIN,   "TTIN"   },
    { SIGTTOU,   "TTOU"   },
    /* ADR-0044: the rest of the common POSIX/BSD signals agents reference. */
#ifdef SIGURG
    { SIGURG,    "URG"    },
#endif
#ifdef SIGXCPU
    { SIGXCPU,   "XCPU"   },
#endif
#ifdef SIGXFSZ
    { SIGXFSZ,   "XFSZ"   },
#endif
#ifdef SIGVTALRM
    { SIGVTALRM, "VTALRM" },
#endif
#ifdef SIGPROF
    { SIGPROF,   "PROF"   },
#endif
#ifdef SIGWINCH
    { SIGWINCH,  "WINCH"  },
#endif
#ifdef SIGIO
    { SIGIO,     "IO"     },
#endif
#ifdef SIGSYS
    { SIGSYS,    "SYS"    },
#endif
#ifdef SIGINFO
    { SIGINFO,   "INFO"   },
#endif
    { 0, NULL }
};

/* ── Critical process protection list ─────────────────────────────────────── */

static const char* CRITICAL_PROCESSES[] = {
    "init",
    "systemd",
    "sshd",
    "kthreadd",
    "kworker",
    "ksoftirqd",
    "migration",
    "watchdog",
    "rcu_sched",
    "rcu_bh",
    "rcu_preempt",
    "cpuhp",
    "idle",
    "kdevtmpfs",
    "kswapd",
    "khugepaged",
    "kcompactd",
    "jash",          /* don't kill ourselves */
    NULL
};

/* ── Helpers ──────────────────────────────────────────────────────────────── */

/* Simple string equality (no libc dependency beyond what's available). */
static int kill_streq(const char* a, const char* b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

/* Case-insensitive substring check: does haystack contain needle? */
static int kill_strcasestr(const char* haystack, const char* needle) {
    if (!needle || !*needle) return 1;
    if (!haystack) return 0;
    size_t nlen = strlen(needle);
    size_t hlen = strlen(haystack);
    if (nlen > hlen) return 0;
    for (size_t i = 0; i <= hlen - nlen; i++) {
        size_t j;
        for (j = 0; j < nlen; j++) {
            char hc = haystack[i + j];
            char nc = needle[j];
            if (hc >= 'A' && hc <= 'Z') hc += 32;
            if (nc >= 'A' && nc <= 'Z') nc += 32;
            if (hc != nc) break;
        }
        if (j == nlen) return 1;
    }
    return 0;
}

/* Check if a process name is critical. */
static int kill_is_critical(const char* comm) {
    if (!comm) return 0;
    for (int i = 0; CRITICAL_PROCESSES[i]; i++) {
        /* Prefix match for kernel threads (kworker/0:0 etc) */
        size_t clen = strlen(CRITICAL_PROCESSES[i]);
        if (strncmp(comm, CRITICAL_PROCESSES[i], clen) == 0)
            return 1;
    }
    return 0;
}

/* Read process name via OSIF (was /proc/[pid]/comm). */
static int kill_read_comm(pid_t pid, char* buf, size_t bufsz) {
    osif_procinfo_t info;
    if (bear_osinfo_proc_get((int)pid, &info) != 0) return -1;
    if (!info.comm[0]) return -1;
    strlcpy(buf, info.comm, bufsz);
    return 0;
}

/* Check if a string is purely numeric (a PID). */
static int kill_is_numeric(const char* s) {
    if (!s || !*s) return 0;
    if (*s == '-') s++;  /* allow negative (for kill -9 etc, but not here) */
    if (!*s) return 0;
    while (*s) {
        if (*s < '0' || *s > '9') return 0;
        s++;
    }
    return 1;
}

static int kill_signal_is_reserved(int sig) {
#if defined(__linux__) && defined(SIGRTMIN)
    /* Linux reserves every kernel realtime slot below libc's SIGRTMIN:
     * glibc reserves 32/33, while musl also reserves 34. */
    return sig >= 32 && sig < SIGRTMIN;
#else
    (void)sig;
    return 0;
#endif
}

static int kill_signal_limit(void) {
    int signal_limit = 1;
#if defined(NSIG)
    signal_limit = NSIG;
#elif defined(_NSIG)
    signal_limit = _NSIG;
#elif defined(SIGRTMAX)
    signal_limit = SIGRTMAX;
    if (signal_limit < INT_MAX) signal_limit++;
#else
    for (int i = 0; KILL_SIGNALS[i].name; i++)
        if (KILL_SIGNALS[i].num >= signal_limit && KILL_SIGNALS[i].num < INT_MAX)
            signal_limit = KILL_SIGNALS[i].num + 1;
#endif
#if defined(SIGRTMAX)
    int realtime_limit = SIGRTMAX;
    if (realtime_limit < INT_MAX) realtime_limit++;
    if (realtime_limit < signal_limit) signal_limit = realtime_limit;
#endif
    return signal_limit;
}

/* Parse a signal number from a string like "-9", "-SIGKILL", "-KILL", "-15" */
static int kill_parse_signal(const char* arg, int* out_sig) {
    if (!arg || *arg != '-') return 0;
    arg++;  /* skip '-' */

    /* Check for "SIG" prefix */
    const char* name = arg;
    if (strncasecmp(name, "SIG", 3) == 0)
        name += 3;

    /* Try numeric first */
    if (kill_is_numeric(arg)) {
        int sig = 0;
        int signal_limit = kill_signal_limit();
        const char* p = arg;
        while (*p >= '0' && *p <= '9') {
            int digit = *p - '0';
            if (sig > (INT_MAX - digit) / 10) return 0;
            sig = sig * 10 + (*p++ - '0');
        }
        if (sig >= 0 && sig < signal_limit) {
            if (kill_signal_is_reserved(sig)) return 0;
            *out_sig = sig;
            return 1;
        }
        return 0;
    }

    /* Try signal name */
    for (int i = 0; KILL_SIGNALS[i].name; i++) {
        if (strcasecmp(KILL_SIGNALS[i].name, name) == 0) {
            *out_sig = KILL_SIGNALS[i].num;
            return 1;
        }
    }
    return 0;
}

/* List all signal names. */
static void kill_list_signals(jash_writer_t w, void* ctx) {
    for (int i = 0; KILL_SIGNALS[i].name; i++) {
        jash_writef(w, ctx, " %2d) SIG%-6s", KILL_SIGNALS[i].num, KILL_SIGNALS[i].name);
        if ((i + 1) % 5 == 0)
            jash_writes(w, ctx, "\n");
    }
    jash_writes(w, ctx, "\n");
}

/* ── Core kill logic ──────────────────────────────────────────────────────── */

/* Maximum processes we can batch-kill in one invocation. */
#define KILL_MAX_TARGETS 256

typedef struct {
    pid_t pid;
    char  comm[64];
} kill_target_t;

/* Kill (or dry-run) a single target. Returns 0 on success, 1 on failure. */
static int kill_one(kill_target_t* t, int sig, int dry_run, int safe_mode,
                     jash_writer_t w, void* ctx) {
    /* PID 0 and negatives target process groups (and -1 broadcasts). Refuse
     * before comm inspection and, critically, before sys_kill. */
    if (t->pid == 0) {
        jash_writes(w, ctx, "kill: refusing PID 0 (process-group kill)\n");
        return 1;
    }
    if (t->pid < 0) {
        jash_writef(w, ctx,
            "kill: refusing negative PID %d (process-group/broadcast kill)\n",
            (int)t->pid);
        return 1;
    }

    /* Safety check */
    if (safe_mode && kill_is_critical(t->comm)) {
        jash_writef(w, ctx, "kill: blocked critical process: %s (PID %d)\n",
                    t->comm, (int)t->pid);
        return 1;
    }

    /* Also always protect PID 1 and our own PID */
    if (t->pid == 1) {
        jash_writes(w, ctx, "kill: cannot kill init (PID 1)\n");
        return 1;
    }
    if (t->pid == getpid()) {
        jash_writes(w, ctx, "kill: cannot kill self\n");
        return 1;
    }

    if (dry_run) {
        jash_writef(w, ctx, "  Would kill PID %d (%s) with signal %d\n",
                    (int)t->pid, t->comm[0] ? t->comm : "unknown", sig);
        return 0;
    }

    long ret = sys_kill((long)t->pid, sig);
    if (ret < 0) {
        jash_writef(w, ctx, "kill: failed to kill PID %d (%s): %s\n",
                    (int)t->pid, t->comm[0] ? t->comm : "unknown",
                    strerror(errno));
        return 1;
    }
    /* Confirmation → STDERR (GNU kill is silent on success; keep stdout clean
     * so `x=$(kill $p)` captures nothing). */
    jash_writef(jash_stderr_writer, 0, "kill: sent signal %d to PID %d (%s)\n",
                sig, (int)t->pid, t->comm[0] ? t->comm : "unknown");
    return 0;
}

/* ── Smart process suggestion (when kill called without args) ─────────────── */

typedef struct {
    pid_t pid;
    char  comm[64];
    unsigned long cpu_time;  /* in jiffies */
    unsigned long rss_pages; /* resident set size in pages */
} proc_info_t;

/* Suggest processes to kill when no args provided — OSIF-portable. */
typedef struct {
    proc_info_t* procs;
    int*         nprocs;
    pid_t        self;
} kill_suggest_ctx_t;

static int kill_suggest_cb(const osif_procinfo_t* oi, void* user) {
    kill_suggest_ctx_t* c = (kill_suggest_ctx_t*)user;
    if (*c->nprocs >= 64) return 1;
    if (oi->pid == 1 || (pid_t)oi->pid == c->self) return 0;
    if (!oi->comm[0]) return 0;
    /* Skip kernel threads (heuristic: leading 'k' + lowercase) */
    if (oi->comm[0] == 'k' && islower((unsigned char)oi->comm[1])) return 0;
    proc_info_t* p = &c->procs[(*c->nprocs)++];
    p->pid = (pid_t)oi->pid;
    strlcpy(p->comm, oi->comm, sizeof(p->comm));
    p->cpu_time  = oi->cpu_ticks;
    p->rss_pages = oi->rss_kb;   /* field reused as kB since OSIF */
    return 0;
}

typedef struct {
    kill_target_t* targets;
    int*           ntargets;
    const char*    pattern;
} kill_match_ctx_t;

static int kill_match_cb(const osif_procinfo_t* oi, void* user) {
    kill_match_ctx_t* c = (kill_match_ctx_t*)user;
    if (*c->ntargets >= KILL_MAX_TARGETS) return 1;
    if (!oi->comm[0]) return 0;
    if (kill_streq(oi->comm, c->pattern) ||
        kill_strcasestr(oi->comm, c->pattern)) {
        kill_target_t* t = &c->targets[(*c->ntargets)++];
        t->pid = (pid_t)oi->pid;
        strlcpy(t->comm, oi->comm, sizeof(t->comm));
    }
    return 0;
}

static void kill_suggest_processes(jash_writer_t w, void* ctx) {
    if (!(bear_osinfo_caps() & OSIF_CAP_PROC)) {
        jash_writes(w, ctx,
            "kill: process suggestion not available on this OS "
            "(OSIF_CAP_PROC off)\n");
        return;
    }

    proc_info_t procs[64];
    int nprocs = 0;
    kill_suggest_ctx_t cctx = { procs, &nprocs, getpid() };
    if (bear_osinfo_proc_iter(kill_suggest_cb, &cctx) != 0 || nprocs == 0) {
        jash_writes(w, ctx, "kill: no processes found\n");
        return;
    }

    /* Find top CPU consumer */
    int top_cpu_idx = 0;
    for (int i = 1; i < nprocs; i++) {
        if (procs[i].cpu_time > procs[top_cpu_idx].cpu_time)
            top_cpu_idx = i;
    }

    /* Find top memory consumer */
    int top_mem_idx = 0;
    for (int i = 1; i < nprocs; i++) {
        if (procs[i].rss_pages > procs[top_mem_idx].rss_pages)
            top_mem_idx = i;
    }

    jash_writes(w, ctx, "\n");
    jash_writes(w, ctx, "Top resource consumers:\n");
    jash_writes(w, ctx, "\n");

    if (top_cpu_idx != top_mem_idx) {
        jash_writef(w, ctx, "  CPU: PID %d (%s) - %lu ticks\n",
                    (int)procs[top_cpu_idx].pid,
                    procs[top_cpu_idx].comm,
                    procs[top_cpu_idx].cpu_time);
        jash_writef(w, ctx, "  MEM: PID %d (%s) - %lu kB\n",
                    (int)procs[top_mem_idx].pid,
                    procs[top_mem_idx].comm,
                    procs[top_mem_idx].rss_pages);
    } else {
        jash_writef(w, ctx, "  TOP: PID %d (%s) - %lu ticks, %lu kB\n",
                    (int)procs[top_cpu_idx].pid,
                    procs[top_cpu_idx].comm,
                    procs[top_cpu_idx].cpu_time,
                    procs[top_cpu_idx].rss_pages);
    }

    jash_writes(w, ctx, "\n");
    jash_writes(w, ctx, "Usage: kill <pid> | kill <name> | kill <pattern>\n");
    jash_writes(w, ctx, "       kill --dry-run <target> (preview without killing)\n");
}

/* ── Main builtin entry ───────────────────────────────────────────────────── */

int jash_b_kill(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    (void)s;

    if (!args || !*args) {
        /* No args: suggest processes to kill */
        kill_suggest_processes(w, ctx);
        return 0;
    }

    /* ── Parse arguments ──────────────────────────────────────────────────── */
    int signal = SIGTERM;
    int dry_run = 0;
    int safe_mode = 1;  /* safety ON by default */
    const char* target = NULL;

    /* Tokenize args into argv for easier parsing. */
    char args_copy[1024];
    strlcpy(args_copy, args, sizeof(args_copy));

    char* argv[32];
    int argc = 0;
    char* tok = strtok(args_copy, " \t");
    while (tok && argc < 31) {
        argv[argc++] = tok;
        tok = strtok(NULL, " \t");
    }
    argv[argc] = NULL;

    if (argc == 0) {
        jash_writes(w, ctx, "kill: no target specified\n");
        return 1;
    }

    /* Parse flags and signal */
    int argi = 0;
    int end_opts = 0;
    while (argi < argc) {
        if (!end_opts && kill_streq(argv[argi], "--")) { end_opts = 1; argi++; continue; }
        if (!end_opts && (kill_streq(argv[argi], "-l") || kill_streq(argv[argi], "--list"))) {
            /* -l NUM → the signal's name; -l NAME → its number (GNU). Bare -l
             * lists all. `kill -l 139` (a wait-status) → the signal of 139-128. */
            if (argi + 1 < argc) {
                const char* a = argv[argi + 1];
                if (kill_is_numeric(a)) {
                    int n = atoi(a);
                    if (n > 128) n -= 128;   /* exit-status form */
                    for (int i = 0; KILL_SIGNALS[i].name; i++)
                        if (KILL_SIGNALS[i].num == n) { jash_writef(w, ctx, "%s\n", KILL_SIGNALS[i].name); return 0; }
                    jash_writef(jash_stderr_writer, 0, "kill: %d: invalid signal specification\n", n);
                    return 1;
                }
                /* name → number */
                const char* nm = a; if (strncasecmp(nm, "SIG", 3) == 0) nm += 3;
                for (int i = 0; KILL_SIGNALS[i].name; i++)
                    if (strcasecmp(KILL_SIGNALS[i].name, nm) == 0) { jash_writef(w, ctx, "%d\n", KILL_SIGNALS[i].num); return 0; }
                jash_writef(jash_stderr_writer, 0, "kill: %s: invalid signal specification\n", a);
                return 1;
            }
            kill_list_signals(w, ctx);
            return 0;
        }
        if (!end_opts && kill_streq(argv[argi], "-s") && argi + 1 < argc) {
            /* -s SIGNAL: named or numeric (kill -s TERM / kill -s 9). */
            char dashed[32]; snprintf(dashed, sizeof dashed, "-%s", argv[argi + 1]);
            int ps;
            if (kill_parse_signal(dashed, &ps)) { signal = ps; argi += 2; continue; }
            jash_writef(jash_stderr_writer, 0, "kill: %s: invalid signal specification\n", argv[argi + 1]);
            return 1;
        }
        if (kill_streq(argv[argi], "--dry-run") || kill_streq(argv[argi], "-n")) {
            dry_run = 1;
            argi++;
            continue;
        }
        if (kill_streq(argv[argi], "--safe")) {
            safe_mode = 1;
            argi++;
            continue;
        }
        if (kill_streq(argv[argi], "--no-safe") || kill_streq(argv[argi], "--unsafe")) {
            safe_mode = 0;
            argi++;
            continue;
        }
        if (kill_streq(argv[argi], "-h") || kill_streq(argv[argi], "--help")) {
            jash_writes(w, ctx, "Usage: kill [-signal] [--dry-run] [--safe] <pid|name|pattern>\n");
            jash_writes(w, ctx, "\n");
            jash_writes(w, ctx, "Options:\n");
            jash_writes(w, ctx, "  -SIGNAL        Signal to send (default: SIGTERM)\n");
            jash_writes(w, ctx, "  -9, -KILL      Send SIGKILL\n");
            jash_writes(w, ctx, "  --dry-run, -n  Show what would be killed without sending signals\n");
            jash_writes(w, ctx, "  --safe         Block critical system processes (default: on)\n");
            jash_writes(w, ctx, "  --unsafe       Allow killing critical system processes\n");
            jash_writes(w, ctx, "  -l, --list     List signal names\n");
            jash_writes(w, ctx, "\n");
            jash_writes(w, ctx, "Targets:\n");
            jash_writes(w, ctx, "  <pid>          Kill process by PID\n");
            jash_writes(w, ctx, "  <name>         Kill all processes matching name (reads /proc/*/comm)\n");
            jash_writes(w, ctx, "  <pattern>      Kill all processes whose comm contains pattern\n");
            return 0;
        }
        /* Try to parse as signal specifier (unless after --). */
        if (!end_opts && argv[argi][0] == '-' && argv[argi][1] != '\0') {
            int parsed_sig;
            if (kill_parse_signal(argv[argi], &parsed_sig)) {
                signal = parsed_sig;
                argi++;
                continue;
            }
            /* Unknown flag */
            jash_writef(jash_stderr_writer, 0, "kill: unknown option '%s'\n", argv[argi]);
            return 1;
        }
        /* First non-flag argument is the target */
        target = argv[argi];
        argi++;
        break;
    }

    if (!target && argi < argc) {
        target = argv[argi++];
    }

    if (!target) {
        jash_writes(w, ctx, "kill: no target specified\n");
        return 1;
    }

    /* ── Build target list ────────────────────────────────────────────────── */
    kill_target_t targets[KILL_MAX_TARGETS];
    int ntargets = 0;

    if (kill_is_numeric(target)) {
        /* Traditional PID kill */
        pid_t pid = (pid_t)atol(target);
        targets[ntargets].pid = pid;
        targets[ntargets].comm[0] = '\0';
        kill_read_comm(pid, targets[ntargets].comm, sizeof(targets[ntargets].comm));
        ntargets++;

        /* Additional PIDs from remaining args */
        while (argi < argc && ntargets < KILL_MAX_TARGETS) {
            if (kill_is_numeric(argv[argi])) {
                pid = (pid_t)atol(argv[argi]);
                targets[ntargets].pid = pid;
                targets[ntargets].comm[0] = '\0';
                kill_read_comm(pid, targets[ntargets].comm,
                               sizeof(targets[ntargets].comm));
                ntargets++;
            }
            argi++;
        }
    } else {
        /* Name-based / pattern-based killing.
         * Scan /proc to find all matching processes. */
/* Name-based matching via OSIF iterator — portable (ADR-0017). */
        if (!(bear_osinfo_caps() & OSIF_CAP_PROC)) {
            jash_writes(w, ctx,
                "kill: name-based killing not available on this OS "
                "(OSIF_CAP_PROC off) — use a numeric PID\n");
            return 1;
        }
        kill_match_ctx_t mctx = { targets, &ntargets, target };
        bear_osinfo_proc_iter(kill_match_cb, &mctx);
    }

    if (ntargets == 0) {
        jash_writef(w, ctx, "kill: no processes found matching '%s'\n", target);
        return 1;
    }

    /* ── Dry-run summary ──────────────────────────────────────────────────── */
    if (dry_run) {
        jash_writef(w, ctx, "Would kill %d process(es) with signal %d:\n",
                    ntargets, signal);
    }

    /* ── Execute kills ────────────────────────────────────────────────────── */
    int failures = 0;
    for (int i = 0; i < ntargets; i++) {
        failures += kill_one(&targets[i], signal, dry_run, safe_mode, w, ctx);
    }

    return failures ? 1 : 0;
}

/* Structured entry retains argv and observes the actual syscall outcome. */
typedef struct {
    kill_target_t values[KILL_MAX_TARGETS];
    size_t count;
    const char* pattern;
    int overflow;
} kill_native_targets_t;
static int kill_native_collect(const osif_procinfo_t* info, void* context) {
    kill_native_targets_t* targets = context;
    if (!kill_strcasestr(info->comm, targets->pattern)) return 0;
    if (targets->count == KILL_MAX_TARGETS) { targets->overflow = 1; return 1; }
    kill_target_t* target = &targets->values[targets->count++];
    target->pid = info->pid;
    strlcpy(target->comm, info->comm, sizeof target->comm);
    return 0;
}
static void kill_native_row(jash_discovery_rows_t* rows, const char* kind,
        const kill_target_t* target, int sig, int preview, int sent,
        const char* error, int error_number) {
#define KF(name, val) {JDV_LITERAL(name), (val)}
    jdv_record_input_t fields[] = {
        KF("kind", jash_discovery_string(rows, kind)),
        KF("pid", target ? jdv_value_i64(target->pid) : jdv_value_nothing()),
        KF("comm", jash_discovery_string(rows, target && target->comm[0] ? target->comm : NULL)),
        KF("signal", jdv_value_u64((uint64_t)sig)), KF("preview", jdv_value_bool(preview)),
        KF("sent", jdv_value_bool(sent)), KF("errno", error_number ? jdv_value_i64(error_number) : jdv_value_nothing()),
        KF("error", jash_discovery_string(rows, error))
    };
    jash_discovery_add(rows, fields, sizeof fields / sizeof *fields);
#undef KF
}
int jash_b_kill_argv(jash_state_t* state, jash_writer_t writer, void* context, int argc, char** argv) {
    int mode, selected = jash_discovery_format(argc, argv, &mode);
    if (!selected) return jash_discovery_text(jash_b_kill, state, writer, context, argc, argv);
    if (selected < 0) return jash_discovery_error(writer, context, "kill", mode, "conflicting_formats", 2);
    int sig = SIGTERM, preview = 0, safe = 1, list = 0, ended = 0;
    kill_native_targets_t targets = {0};
    const char* error = NULL;
    for (int i = 0; i < argc && !error; i++) {
        const char* arg = argv[i];
        if (!ended && !strcmp(arg, "--")) { ended = 1; continue; }
        if (!ended && jash_discovery_format_flag(arg)) continue;
        if (!ended && (!strcmp(arg, "-l") || !strcmp(arg, "--list"))) { list = 1; continue; }
        if (!ended && (!strcmp(arg, "--dry-run") || !strcmp(arg, "-n"))) { preview = 1; continue; }
        if (!ended && !strcmp(arg, "--safe")) { safe = 1; continue; }
        if (!ended && (!strcmp(arg, "--unsafe") || !strcmp(arg, "--no-safe"))) { safe = 0; continue; }
        if (!ended && !strcmp(arg, "-s")) {
            if (++i == argc || strlen(argv[i]) >= 30) { error = "invalid_signal"; break; }
            char named[32]; snprintf(named, sizeof named, "-%s", argv[i]);
            if (!kill_parse_signal(named, &sig)) error = "invalid_signal";
            continue;
        }
        if (!ended && arg[0] == '-' && !list) {
            if (!kill_parse_signal(arg, &sig)) error = "invalid_options";
            continue;
        }
        if (list) {
            if (targets.pattern || targets.count) { error = "too_many_signal_operands"; break; }
            targets.pattern = arg;
        } else if (kill_is_numeric(arg)) {
            char* end; errno = 0; long pid = strtol(arg, &end, 10);
            if (errno || *end || pid > INT_MAX || pid <= 0 || targets.count == KILL_MAX_TARGETS) { error = "invalid_pid_or_target_limit"; break; }
            kill_target_t* target = &targets.values[targets.count++];
            target->pid = (pid_t)pid; target->comm[0] = 0;
            (void)kill_read_comm(target->pid, target->comm, sizeof target->comm);
        } else {
            if (targets.pattern || targets.count) { error = "mixed_or_multiple_patterns"; break; }
            targets.pattern = arg;
        }
    }
    if (!error && !list && targets.pattern) {
        if (!(bear_osinfo_caps() & OSIF_CAP_PROC)) error = "process_provider_unavailable";
        else if (bear_osinfo_proc_iter(kill_native_collect, &targets)) error = "process_observation_failed";
        else if (targets.overflow) error = "target_limit";
    }
    if (error) return jash_discovery_error(writer, context, "kill", mode, error, 2);
    jash_discovery_rows_t rows;
    if (jash_discovery_rows_init(&rows)) return jash_discovery_error(writer, context, "kill", mode, "allocation_failed", 125);
    int status = 0;
    if (list) {
        int selected_signal = -1;
        if (targets.pattern) {
            if (kill_is_numeric(targets.pattern)) {
                char* end; errno = 0; long n = strtol(targets.pattern, &end, 10);
                if (!errno && !*end && n >= 0 && n < 256) selected_signal = (int)(n > 128 ? n - 128 : n);
            } else {
                char named[64];
                if (snprintf(named, sizeof named, "-%s", targets.pattern) < (int)sizeof named)
                    (void)kill_parse_signal(named, &selected_signal);
            }
        }
        for (int i = 0; KILL_SIGNALS[i].name; i++) {
            if (targets.pattern && KILL_SIGNALS[i].num != selected_signal) continue;
            jdv_record_input_t fields[] = {
                {JDV_LITERAL("kind"), jash_discovery_string(&rows, "signal")},
                {JDV_LITERAL("signal"), jdv_value_u64((uint64_t)KILL_SIGNALS[i].num)},
                {JDV_LITERAL("name"), jash_discovery_string(&rows, KILL_SIGNALS[i].name)}
            };
            jash_discovery_add(&rows, fields, 3);
        }
        if (!rows.count) status = 1;
    } else if (!targets.count) {
        kill_native_row(&rows, "error", NULL, sig, preview, 0, "no_matching_process", 0); status = 1;
    } else {
        if (!preview) {
            int admission = jash_discovery_effects_begin(&rows, writer, context, "kill", mode, "jash.kill.v1");
            if (admission) { jash_discovery_rows_free(&rows); return admission; }
        }
        for (size_t i = 0; i < targets.count && !rows.error; i++) {
            kill_target_t* target = &targets.values[i];
            int number = 0, sent = 0;
            const char* failure = target->pid <= 1 ? "protected_pid" : target->pid == getpid() ? "protected_self" :
                safe && kill_is_critical(target->comm) ? "protected_process" : NULL;
            if (!failure && !preview) {
                if (sys_kill(target->pid, sig)) { number = errno; failure = "signal_failed"; }
                else sent = sig != 0;
            }
            kill_native_row(&rows, "signal_result", target, sig, preview, sent, failure, number);
            if (failure) status = 1;
            if (jash_discovery_effects_flush(&rows)) { status = 125; break; }
        }
    }
    int result = jash_discovery_emit_schema(writer, context, "kill", mode, &rows, status, "jash.kill.v1");
    jash_discovery_rows_free(&rows); return result;
}
