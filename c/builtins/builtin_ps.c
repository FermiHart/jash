// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// 🎵 Hear F E R M I ∞ H A R T music. No obligation. Just a wish.
//
// Module: builtins/ps — Linux-standard ps with colors, unicode, and bars
//
// Features:
//   ps              — list processes (Linux ps aux style)
//   ps aux          — full detail (USER, PID, %CPU, %MEM, VSZ, RSS, SURFACE, STAT, START, TIME, COMM)
//   ps -ef          — System V style
//   ps --tree       — tree view with unicode box-drawing
//   ps --top N      — top N by CPU/memory
//   ps --pattern X  — filter by name
//   ps --cache      — use cached results (TTL 5s)
//
// Colors:
//   Running (R): bright green
//   Sleeping (S): cyan
//   Disk wait (D): red
//   Zombie (Z): yellow
//   Stopped (T): magenta
//   Dead (X): dark gray
//
// Unicode:
//   ⚡ Running  💤 Sleeping  🔥 Hot CPU  🧊 Frozen  💀 Zombie  ⏸ Stopped
//
// Bars: CPU and memory usage bars with gradient

#include "builtin.h"
#include "osif/osinfo.h"
#include <core/env.h>
#include <core/agent_json.h>   /* jash_json_esc (--json) */
#include <unistd.h>            /* getpagesize */
#include <core/cache.h>
#include <core/unix_format.h>
#include <core/string_compat.h>
#include <sys/syscalls.h>
#include <theme/colors.h>
#include <dirent.h>
#include <sys/stat.h>
#include <string.h>
#include <time.h>
#include <stdio.h>
#include <ctype.h>
#include <pwd.h>
#include <unistd.h>
#include <fcntl.h>

/* ── Cache infrastructure ─────────────────────────────────────────────────── */

#define PS_CACHE_TTL 5  /* seconds */

static char* g_ps_cache = NULL;
static time_t g_ps_cache_time = 0;

/* ── Process information ──────────────────────────────────────────────────── */

typedef struct {
    pid_t pid;
    pid_t ppid;
    char comm[64];
    char state;
    unsigned long utime;       /* user mode jiffies */
    unsigned long stime;       /* kernel mode jiffies */
    unsigned long cpu_time;    /* utime + stime */
    unsigned long rss_pages;   /* resident set size in pages */
    unsigned long vsz_pages;   /* virtual size in pages */
    unsigned long rss_kb;      /* exact provider value, when valid */
    unsigned long vsz_kb;      /* exact provider value, when valid */
    uid_t uid;
    gid_t gid;
    char tty[16];
    char seat[16];
    int surface;
    int foreground;
    unsigned valid;
    unsigned long start_time;  /* jiffies since boot */
    int nice;
    int num_threads;
} ps_proc_info_t;

#define PS_MAX_PROCS 512
#define PS_PAGE_SIZE 4096

/* ── Color codes ──────────────────────────────────────────────────────────── */

#define PS_COLOR_RESET    "\033[0m"
#define PS_COLOR_BOLD     "\033[1m"
#define PS_COLOR_DIM      "\033[2m"
#define PS_COLOR_RED      "\033[31m"
#define PS_COLOR_GREEN    "\033[32m"
#define PS_COLOR_YELLOW   "\033[33m"
#define PS_COLOR_BLUE     "\033[34m"
#define PS_COLOR_MAGENTA  "\033[35m"
#define PS_COLOR_CYAN     "\033[36m"
#define PS_COLOR_WHITE    "\033[37m"
#define PS_COLOR_BRIGHT_GREEN  "\033[92m"
#define PS_COLOR_BRIGHT_CYAN   "\033[96m"
#define PS_COLOR_BRIGHT_RED    "\033[91m"
#define PS_COLOR_BRIGHT_YELLOW "\033[93m"
#define PS_COLOR_BRIGHT_MAGENTA "\033[95m"
#define PS_COLOR_GRAY     "\033[90m"

/* ── Unicode icons ────────────────────────────────────────────────────────── */

#define PS_ICON_RUNNING   "⚡"
#define PS_ICON_SLEEPING  "💤"
#define PS_ICON_DISK      "💾"
#define PS_ICON_ZOMBIE    "💀"
#define PS_ICON_STOPPED   "⏸"
#define PS_ICON_DEAD      "☠"
#define PS_ICON_UNKNOWN   "❓"
#define PS_ICON_HOT       "🔥"
#define PS_ICON_TREE_L    "└─"
#define PS_ICON_TREE_M    "├─"
#define PS_ICON_TREE_V    "│ "
#define PS_ICON_TREE_SP   "  "

/* ── Get color for process state ──────────────────────────────────────────── */

static const char* ps_state_color(char state) {
    switch (state) {
        case 'R': return PS_COLOR_BRIGHT_GREEN;   /* Running */
        case 'S': return PS_COLOR_BRIGHT_CYAN;    /* Sleeping */
        case 'D': return PS_COLOR_BRIGHT_RED;     /* Disk wait */
        case 'Z': return PS_COLOR_BRIGHT_YELLOW;  /* Zombie */
        case 'T': return PS_COLOR_BRIGHT_MAGENTA; /* Stopped */
        case 'X': case 'x': return PS_COLOR_GRAY; /* Dead */
        default:  return PS_COLOR_WHITE;
    }
}

/* ── Get icon for process state ───────────────────────────────────────────── */

static const char* ps_state_icon(char state) {
    switch (state) {
        case 'R': return PS_ICON_RUNNING;
        case 'S': return PS_ICON_SLEEPING;
        case 'D': return PS_ICON_DISK;
        case 'Z': return PS_ICON_ZOMBIE;
        case 'T': return PS_ICON_STOPPED;
        case 'X': case 'x': return PS_ICON_DEAD;
        default:  return PS_ICON_UNKNOWN;
    }
}

/* ── Get STAT string (like Linux ps) ──────────────────────────────────────── */

static const char* ps_stat_string(char state, int foreground) {
    switch (state) {
        case 'R': return foreground ? "R+" : "R";
        case 'S': return "S";     /* Sleeping */
        case 'D': return "D";     /* Disk sleep */
        case 'Z': return "Z";     /* Zombie */
        case 'T': return "T";     /* Stopped */
        case 'X': return "X";     /* Dead */
        case 'I': return "I";     /* Idle kernel thread */
        default:  return "?";
    }
}

/* ── Process info via OSIF L1 (ADR-0017 wave 2) ─────────────────────────────
 * The /proc parser moved to c/osif/linux/osinfo.c; macOS uses libproc,
 * MINIX will use /proc/[pid]/psinfo. This file is now OS-agnostic. */

static long ps_page_kb(void) {
    static long kb = 0;
    if (!kb) {
        long ps = sysconf(_SC_PAGESIZE);
        kb = (ps > 0 ? ps : 4096) / 1024;
        if (kb < 1) kb = 1;
    }
    return kb;
}

static void ps_from_osif(const osif_procinfo_t* oi, ps_proc_info_t* info) {
    unsigned valid = oi->valid;
    int legacy = strcmp(bear_osinfo_target(), "tina") != 0;

    memset(info, 0, sizeof(*info));
    info->pid   = (pid_t)oi->pid;
    info->ppid  = (pid_t)oi->ppid;
    info->uid   = (uid_t)oi->uid;
    info->state = oi->state;
    strlcpy(info->comm, oi->comm, sizeof(info->comm));
    strlcpy(info->tty,  oi->tty,  sizeof(info->tty));
    strlcpy(info->seat, oi->seat[0] ? oi->seat : "-", sizeof(info->seat));
    info->surface = legacy ? -1 : oi->surface;
    info->foreground = oi->surface_foreground;
    /* Older OSIF providers predate validity bits; retain their measured
     * fields while Tina stays fail-closed for absent Proc-NU values. */
    if (legacy)
        valid |= OSIF_PROC_VALID_CPU | OSIF_PROC_VALID_RSS |
                 OSIF_PROC_VALID_VSZ | OSIF_PROC_VALID_START |
                 OSIF_PROC_VALID_THREADS;
    info->valid = valid;
    info->cpu_time    = oi->cpu_ticks;
    info->utime       = oi->cpu_ticks;   /* split unavailable portably */
    info->stime       = 0;
    info->rss_kb      = oi->rss_kb;
    info->vsz_kb      = oi->vsz_kb;
    info->rss_pages   = oi->rss_kb / (unsigned long)ps_page_kb();
    info->vsz_pages   = oi->vsz_kb / (unsigned long)ps_page_kb();
    info->start_time  = oi->start_ticks;
    info->num_threads = oi->num_threads;
}

/* Keep the native Surface index distinct from the human seat name. */
static void ps_surface_label(const ps_proc_info_t* info, char* buf, size_t bufsz) {
    if (info->surface >= 0) {
        snprintf(buf, bufsz, "Surface %d", info->surface);
    } else if (!strcmp(info->seat, "multi")) {
        strlcpy(buf, "multi", bufsz);
    } else {
        strlcpy(buf, "-", bufsz);
    }
}

/* Collection callback for bear_osinfo_proc_iter — C has no closures, so
 * the filter state travels in a ctx struct. */
typedef struct {
    ps_proc_info_t* procs;
    int*            nprocs;
    int             show_all, show_full;
    const char*     pattern;
    const pid_t*    pid_filter;   /* -p PID[,PID...] (NULL = no filter) */
    int             npid_filter;
} ps_collect_ctx_t;

static int ps_is_kernel_thread(const ps_proc_info_t* info);  /* fwd */

static int ps_collect_cb(const osif_procinfo_t* oi, void* user) {
    ps_collect_ctx_t* c = (ps_collect_ctx_t*)user;
    if (*c->nprocs >= PS_MAX_PROCS) return 1;       /* stop iteration */
    ps_proc_info_t info;
    ps_from_osif(oi, &info);
    if (c->npid_filter > 0) {
        /* Explicit -p list: keep only matching pids (kernel threads too). */
        int match = 0;
        for (int k = 0; k < c->npid_filter; k++)
            if (info.pid == c->pid_filter[k]) { match = 1; break; }
        if (!match) return 0;
    } else if (!c->show_all && !c->show_full && ps_is_kernel_thread(&info)) {
        return 0;
    }
    if (c->pattern && !jash_strcasestr_portable(info.comm, c->pattern))
        return 0;
    c->procs[(*c->nprocs)++] = info;
    return 0;
}

/* ── Check if process is kernel thread ────────────────────────────────────── */

static int ps_is_kernel_thread(const ps_proc_info_t* info) {
    if (info->ppid == 2 || info->pid == 2) return 1;
    if (info->comm[0] == 'k' && info->comm[1] && islower(info->comm[1])) {
        return 1;
    }
    return 0;
}

/* ── Format time from jiffies ─────────────────────────────────────────────── */

static void ps_format_time(unsigned long jiffies, char* buf, size_t bufsz) {
    long hz = sysconf(_SC_CLK_TCK);
    if (hz <= 0) hz = 100;
    unsigned long seconds = jiffies / (unsigned long)hz;
    unsigned long minutes = seconds / 60;
    unsigned long hours = minutes / 60;
    
    seconds %= 60;
    minutes %= 60;
    
    if (hours > 0) {
        snprintf(buf, bufsz, "%lu:%02lu:%02lu", hours, minutes, seconds);
    } else {
        snprintf(buf, bufsz, "%lu:%02lu", minutes, seconds);
    }
}

/* ── Format size ──────────────────────────────────────────────────────────── */

static void ps_format_size(unsigned long kb, char* buf, size_t bufsz) {
    if (kb < 1024) {
        snprintf(buf, bufsz, "%lu", kb);
    } else if (kb < 1024 * 1024) {
        snprintf(buf, bufsz, "%.1fM", kb / 1024.0);
    } else {
        snprintf(buf, bufsz, "%.1fG", kb / (1024.0 * 1024.0));
    }
}

/* ── Get username ─────────────────────────────────────────────────────────── */

static const char* ps_get_username(uid_t uid) {
    struct passwd* pw = getpwuid(uid);
    return pw ? pw->pw_name : "unknown";
}

/* ── Print bar (used in top mode) ─────────────────────────────────────────── */

static void ps_print_bar(int percent, int width, const char* color,
                         jash_writer_t w, void* ctx) __attribute__((unused));

static void ps_print_bar(int percent, int width, const char* color,
                         jash_writer_t w, void* ctx) {
    int filled = (width * percent) / 100;
    if (filled > width) filled = width;
    
    jash_writes(w, ctx, color);
    for (int i = 0; i < filled; i++) {
        double pos = (double)i / (double)width;
        if (pos < 0.3) {
            jash_writes(w, ctx, "█");
        } else if (pos < 0.6) {
            jash_writes(w, ctx, "▓");
        } else if (pos < 0.85) {
            jash_writes(w, ctx, "▒");
        } else {
            jash_writes(w, ctx, "░");
        }
    }
    for (int i = filled; i < width; i++) {
        jash_writes(w, ctx, " ");
    }
    jash_writes(w, ctx, PS_COLOR_RESET);
}

/* ── Tree view ────────────────────────────────────────────────────────────── */

#define PS_MAX_TREE_DEPTH 20

static void ps_print_tree(ps_proc_info_t* procs, int nprocs, pid_t parent,
                          int depth, int* is_last, jash_writer_t w, void* ctx,
                          int piped) {
    if (depth > PS_MAX_TREE_DEPTH) return;
    
    for (int i = 0; i < nprocs; i++) {
        if (procs[i].ppid != parent) continue;
        
        /* Check if this is the last child */
        int last = 1;
        for (int j = i + 1; j < nprocs; j++) {
            if (procs[j].ppid == parent) { last = 0; break; }
        }
        
        /* Print tree prefix */
        for (int d = 0; d < depth - 1; d++) {
            jash_writes(w, ctx, is_last[d] ? PS_ICON_TREE_SP : PS_ICON_TREE_V);
        }
        jash_writes(w, ctx, last ? PS_ICON_TREE_L : PS_ICON_TREE_M);
        
        /* Print process info */
        char line[512];
        char rss_kb[24];
        if (procs[i].valid & OSIF_PROC_VALID_RSS)
            snprintf(rss_kb, sizeof(rss_kb), "%luKB", procs[i].rss_kb);
        else strlcpy(rss_kb, "-", sizeof(rss_kb));
        const char* color = piped ? "" : ps_state_color(procs[i].state);
        const char* icon = piped ? "" : ps_state_icon(procs[i].state);
        const char* reset = piped ? "" : PS_COLOR_RESET;
        
        if (piped) {
            snprintf(line, sizeof(line), " PID %d %s %c %s\n",
                     (int)procs[i].pid, procs[i].comm,
                     procs[i].state, rss_kb);
        } else {
            snprintf(line, sizeof(line), " %s%s%s PID %d %s%s%s (%c) %s%s\n",
                     color, icon, reset,
                     (int)procs[i].pid, color, procs[i].comm, reset,
                     procs[i].state, rss_kb, reset);
        }
        jash_writes(w, ctx, line);
        
        /* Store last status for this depth */
        int saved_last = is_last[depth];
        is_last[depth] = last;
        
        /* Recurse for children */
        ps_print_tree(procs, nprocs, procs[i].pid, depth + 1, is_last, w, ctx, piped);
        
        is_last[depth] = saved_last;
    }
}

/* ── Unicode-aware padding ─────────────────────────────────────────────────── */

/* Count visual width of a UTF-8 string (emoji = 2 cols, ASCII = 1 col) */
static int utf8_visual_width(const char* s) {
    int width = 0;
    while (*s) {
        unsigned char c = (unsigned char)*s;
        if (c < 0x80) {
            width += 1;
            s++;
        } else if ((c & 0xE0) == 0xC0) {
            width += 2;  /* assume wide for 2-byte (could be narrow) */
            s += 2;
        } else if ((c & 0xF0) == 0xE0) {
            width += 2;  /* 3-byte: most emoji are wide */
            s += 3;
        } else if ((c & 0xF8) == 0xF0) {
            width += 2;  /* 4-byte: emoji */
            s += 4;
        } else {
            s++;  /* skip invalid */
        }
    }
    return width;
}

/* Pad string to target visual width, return actual padding needed */
static void ps_pad_visual(char* out, size_t out_sz, const char* s, int target_width) {
    int vwidth = utf8_visual_width(s);
    int pad = target_width - vwidth;
    if (pad < 0) pad = 0;
    int n = snprintf(out, out_sz, "%s", s);
    if (n < 0 || (size_t)n >= out_sz) return;
    for (int i = 0; i < pad && (size_t)(n + i + 1) < out_sz; i++) {
        out[n + i] = ' ';
    }
    out[n + pad] = '\0';
}

/* ── Sort functions ───────────────────────────────────────────────────────── */

typedef int (*ps_cmp_t)(const ps_proc_info_t* a, const ps_proc_info_t* b);

static int ps_cmp_pid(const ps_proc_info_t* a, const ps_proc_info_t* b) {
    return (a->pid < b->pid) ? -1 : (a->pid > b->pid) ? 1 : 0;
}

static int ps_cmp_ppid(const ps_proc_info_t* a, const ps_proc_info_t* b) {
    return (a->ppid < b->ppid) ? -1 : (a->ppid > b->ppid) ? 1 : 0;
}

static int ps_cmp_cpu(const ps_proc_info_t* a, const ps_proc_info_t* b) {
    return (a->cpu_time < b->cpu_time) ? -1 : (a->cpu_time > b->cpu_time) ? 1 : 0;
}

static int ps_cmp_mem(const ps_proc_info_t* a, const ps_proc_info_t* b) {
    return (a->rss_pages < b->rss_pages) ? -1 : (a->rss_pages > b->rss_pages) ? 1 : 0;
}

static int ps_cmp_vsz(const ps_proc_info_t* a, const ps_proc_info_t* b) {
    return (a->vsz_pages < b->vsz_pages) ? -1 : (a->vsz_pages > b->vsz_pages) ? 1 : 0;
}

static int ps_cmp_comm(const ps_proc_info_t* a, const ps_proc_info_t* b) {
    return strcmp(a->comm, b->comm);
}

static int ps_cmp_uid(const ps_proc_info_t* a, const ps_proc_info_t* b) {
    return (a->uid < b->uid) ? -1 : (a->uid > b->uid) ? 1 : 0;
}

static int ps_cmp_state(const ps_proc_info_t* a, const ps_proc_info_t* b) {
    int ca = a->state, cb = b->state;
    return (ca < cb) ? -1 : (ca > cb) ? 1 : 0;
}

static void ps_sort(ps_proc_info_t* procs, int nprocs, ps_cmp_t cmp, int reverse) {
    for (int i = 0; i < nprocs - 1; i++) {
        for (int j = i + 1; j < nprocs; j++) {
            int r = cmp(&procs[i], &procs[j]);
            if (reverse ? (r < 0) : (r > 0)) {
                ps_proc_info_t tmp = procs[i];
                procs[i] = procs[j];
                procs[j] = tmp;
            }
        }
    }
}

/* ── Check if piped ───────────────────────────────────────────────────────── */

static int ps_is_piped(void) {
    return !isatty(STDOUT_FILENO);
}

/* ── Main ps builtin ──────────────────────────────────────────────────────── */

int jash_b_ps(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    (void)s;
    
    /* Parse arguments */
    int tree_mode = 0;
    int top_n = 0;
    int use_cache = 0;
    int show_all = 0;       /* ps aux */
    int show_full = 0;      /* ps -ef */
    const char* pattern = NULL;
    pid_t pid_filter[64];   /* -p PID[,PID...] */
    int   npid_filter = 0;
    const char* o_spec = NULL;  /* -o field[,field...] (e.g. comm=,pid=) */
    char  o_spec_buf[256];      /* stable copy — argv[] dangles after the if-block */
    int   json_out = 0;
    int   reverse_sort = 0;     /* -r / --reverse */
    const char* sort_field = NULL;  /* --sort field */

    if (args) {
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
        
        for (int i = 0; i < argc; i++) {
            if (strcmp(argv[i], "--json") == 0) {
                json_out = 1;
            } else if (strcmp(argv[i], "--tree") == 0 || strcmp(argv[i], "-t") == 0) {
                tree_mode = 1;
            } else if (strcmp(argv[i], "--cache") == 0) {
                use_cache = 1;
            } else if (strcmp(argv[i], "--top") == 0 && i + 1 < argc) {
                top_n = atoi(argv[++i]);
            } else if (strcmp(argv[i], "--pattern") == 0 && i + 1 < argc) {
                pattern = argv[++i];
            } else if (strcmp(argv[i], "-r") == 0 || strcmp(argv[i], "--reverse") == 0) {
                reverse_sort = 1;
            } else if (strcmp(argv[i], "--sort") == 0 && i + 1 < argc) {
                sort_field = argv[++i];
            } else if (strcmp(argv[i], "aux") == 0) {
                show_all = 1;
            } else if (strcmp(argv[i], "-ef") == 0 || strcmp(argv[i], "ef") == 0) {
                show_full = 1;
                show_all = 1;   /* -ef lists ALL processes (System V) */
            /* ADR-0044: all-process selectors — `ps -e`, `ps -A`, and the BSD
             * `ps ax` / `x` / `-ax` forms every agent reaches for. */
            } else if (strcmp(argv[i], "-e") == 0 || strcmp(argv[i], "-A") == 0 ||
                       strcmp(argv[i], "ax") == 0 || strcmp(argv[i], "-ax") == 0 ||
                       strcmp(argv[i], "axu") == 0 ||
                       strcmp(argv[i], "x") == 0 || strcmp(argv[i], "-x") == 0 ||
                       strcmp(argv[i], "a") == 0 || strcmp(argv[i], "-a") == 0) {
                show_all = 1;
            } else if (strcmp(argv[i], "-f") == 0) {
                show_full = 1;
            } else if ((strcmp(argv[i], "-C") == 0) && i + 1 < argc) {
                pattern = argv[++i];   /* -C name: select by command name */
                show_all = 1;
            } else if (strcmp(argv[i], "--") == 0) {
                /* end of options — nothing after ps is an operand here */
            } else if ((strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "-q") == 0 ||
                        strcmp(argv[i], "--pid") == 0) && i + 1 < argc) {
                /* -p PID[,PID...] in the next argument */
                char* p = argv[++i];
                while (p && *p && npid_filter < 64) {
                    pid_filter[npid_filter++] = (pid_t)atoi(p);
                    char* comma = strchr(p, ',');
                    if (!comma) break;
                    p = comma + 1;
                }
            } else if (strncmp(argv[i], "-p", 2) == 0 && argv[i][2]) {
                /* -pPID[,PID...] glued form */
                char* p = argv[i] + 2;
                while (*p && npid_filter < 64) {
                    pid_filter[npid_filter++] = (pid_t)atoi(p);
                    char* comma = strchr(p, ',');
                    if (!comma) break;
                    p = comma + 1;
                }
            } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
                strlcpy(o_spec_buf, argv[++i], sizeof(o_spec_buf));
                o_spec = o_spec_buf;
            } else if (strncmp(argv[i], "-o", 2) == 0 && argv[i][2]) {
                strlcpy(o_spec_buf, argv[i] + 2, sizeof(o_spec_buf));
                o_spec = o_spec_buf;
            } else if (argv[i][0] == '-') {
                jash_writef(w, ctx, "ps: unknown option '%s'\n", argv[i]);
                return 1;
            }
        }
    }
    
    int piped = ps_is_piped();
    
    /* Check cache */
    time_t now = time(NULL);
    if (use_cache && g_ps_cache && (now - g_ps_cache_time < PS_CACHE_TTL)) {
        jash_writes(w, ctx, g_ps_cache);
        return 0;
    }
    
    /* Read all processes via OSIF (ADR-0017): works on every backend.
     * Graceful degradation: if the target lacks OSIF_CAP_PROC, say so
     * honestly and show the shell's own job table instead of nothing. */
    if (!(bear_osinfo_caps() & OSIF_CAP_PROC)) {
        jash_writes(w, ctx,
            "ps: process info not available on this OS "
            "(OSIF_CAP_PROC off) — showing shell jobs only\n");
        return jash_b_jobs(s, w, ctx, "");
    }

    ps_proc_info_t procs[PS_MAX_PROCS];
    int nprocs = 0;
    uid_t my_uid = getuid();

    ps_collect_ctx_t cctx = { procs, &nprocs, show_all, show_full, pattern,
                              pid_filter, npid_filter };
    bear_osinfo_proc_iter(ps_collect_cb, &cctx);

    /* Sort by PID by default; --sort overrides, -r reverses */
    {
        ps_cmp_t cmp = ps_cmp_pid;
        int rev = 0;
        if (sort_field) {
            const char* sf = sort_field;
            if (sf[0] == '-') { rev = 1; sf++; }
            else if (sf[0] == '+') { sf++; }
            if      (strcmp(sf, "pid") == 0)     cmp = ps_cmp_pid;
            else if (strcmp(sf, "ppid") == 0)    cmp = ps_cmp_ppid;
            else if (strcmp(sf, "%cpu") == 0 ||
                     strcmp(sf, "cpu") == 0)     cmp = ps_cmp_cpu;
            else if (strcmp(sf, "%mem") == 0 ||
                     strcmp(sf, "mem") == 0 ||
                     strcmp(sf, "rss") == 0)     cmp = ps_cmp_mem;
            else if (strcmp(sf, "vsz") == 0)     cmp = ps_cmp_vsz;
            else if (strcmp(sf, "comm") == 0 ||
                     strcmp(sf, "cmd") == 0 ||
                     strcmp(sf, "command") == 0) cmp = ps_cmp_comm;
            else if (strcmp(sf, "user") == 0 ||
                     strcmp(sf, "uid") == 0)     cmp = ps_cmp_uid;
            else if (strcmp(sf, "state") == 0 ||
                     strcmp(sf, "stat") == 0)    cmp = ps_cmp_state;
        }
        if (reverse_sort) rev = !rev;
        ps_sort(procs, nprocs, cmp, rev);
    }

    /* Total memory + uptime (for %cpu/%mem columns in -o and aux) */
    long total_mem_kb = 0;
    osif_meminfo_t mi;
    if (bear_osinfo_meminfo(&mi) == 0) total_mem_kb = (long)mi.total_kb;
    double uptime_secs = 0;
    unsigned long up = 0;
    if (bear_osinfo_uptime(&up) == 0) uptime_secs = (double)up;
    long hz = sysconf(_SC_CLK_TCK);

    /* --json: one object per process, isolated from every text path. */
    if (json_out) {
        for (int i = 0; i < nprocs; i++) {
            ps_proc_info_t* p = &procs[i];
            char ec[140]; jash_json_esc(p->comm, ec, sizeof ec);
            char et[40];  jash_json_esc(p->tty[0] ? p->tty : "?", et, sizeof et);
            char es[40];  jash_json_esc(p->seat[0] ? p->seat : "-", es, sizeof es);
            char rss[32], vsz[32], threads[32];
            if (p->valid & OSIF_PROC_VALID_RSS)
                snprintf(rss, sizeof(rss), "%lu", p->rss_kb);
            else
                strlcpy(rss, "null", sizeof(rss));
            if (p->valid & OSIF_PROC_VALID_VSZ)
                snprintf(vsz, sizeof(vsz), "%lu", p->vsz_kb);
            else
                strlcpy(vsz, "null", sizeof(vsz));
            if (p->valid & OSIF_PROC_VALID_THREADS)
                snprintf(threads, sizeof(threads), "%d", p->num_threads);
            else
                strlcpy(threads, "null", sizeof(threads));
            jash_writef(w, ctx,
                "{\"pid\":%d,\"ppid\":%d,\"comm\":\"%s\",\"state\":\"%c\","
                "\"uid\":%u,\"rss_kb\":%s,\"vsz_kb\":%s,\"threads\":%s,"
                "\"tty\":\"%s\",\"surface\":%d,\"seat\":\"%s\"}\n",
                (int)p->pid, (int)p->ppid, ec, p->state ? p->state : '?',
                (unsigned)p->uid,
                rss, vsz, threads, et, p->surface, es);
        }
        return 0;
    }

    /* ── Custom -o field selection (minimal column support) ─────────────────
     * Supports pid, ppid, comm/cmd/command/args, user/uid, stat/state, rss,
     * vsz. A '=' in the spec (e.g. `comm=`) suppresses the header — exactly
     * what `ps -p $$ -o comm=` relies on. */
    if (o_spec) {
        int suppress_hdr = (strchr(o_spec, '=') != NULL);
        char spec[256];
        strlcpy(spec, o_spec, sizeof(spec));
        char* fields[16]; int nfields = 0;
        char* ftok = strtok(spec, ",");
        while (ftok && nfields < 16) {
            char* eq = strchr(ftok, '='); if (eq) *eq = '\0';
            fields[nfields++] = ftok;
            ftok = strtok(NULL, ",");
        }
        if (!suppress_hdr) {
            for (int f = 0; f < nfields; f++) {
                const char* fn = fields[f]; const char* hdr = fn;
                if (!strcmp(fn,"comm")||!strcmp(fn,"cmd")||!strcmp(fn,"command")||!strcmp(fn,"args")) hdr="COMM";
                else if (!strcmp(fn,"pid")) hdr="PID";
                else if (!strcmp(fn,"ppid")) hdr="PPID";
                else if (!strcmp(fn,"user")||!strcmp(fn,"uid")) hdr="USER";
                else if (!strcmp(fn,"stat")||!strcmp(fn,"state")) hdr="STAT";
                else if (!strcmp(fn,"rss")) hdr="RSS";
                else if (!strcmp(fn,"vsz")) hdr="VSZ";
                else if (!strcmp(fn,"surface")) hdr="SURFACE";
                else if (!strcmp(fn,"seat")) hdr="SEAT";
                else if (!strcmp(fn,"start")||!strcmp(fn,"stime")) hdr="START";
                else if (!strcmp(fn,"c")) hdr="C";
                else if (!strcmp(fn,"%cpu")) hdr="%CPU";
                else if (!strcmp(fn,"%mem")) hdr="%MEM";
                jash_writef(w, ctx, "%s%s", f ? " " : "", hdr);
            }
            jash_writes(w, ctx, "\n");
        }
        for (int i = 0; i < nprocs; i++) {
            for (int f = 0; f < nfields; f++) {
                const char* fn = fields[f]; char cell[128];
                if (!strcmp(fn,"comm")||!strcmp(fn,"cmd")||!strcmp(fn,"command")||!strcmp(fn,"args"))
                    snprintf(cell, sizeof(cell), "%s", procs[i].comm);
                else if (!strcmp(fn,"pid"))  snprintf(cell, sizeof(cell), "%d", (int)procs[i].pid);
                else if (!strcmp(fn,"ppid")) snprintf(cell, sizeof(cell), "%d", (int)procs[i].ppid);
                else if (!strcmp(fn,"user")||!strcmp(fn,"uid")) snprintf(cell, sizeof(cell), "%s", ps_get_username(procs[i].uid));
                else if (!strcmp(fn,"stat")||!strcmp(fn,"state")) snprintf(cell, sizeof(cell), "%s", ps_stat_string(procs[i].state, procs[i].foreground));
                else if (!strcmp(fn,"rss")) {
                    if (procs[i].valid & OSIF_PROC_VALID_RSS) snprintf(cell, sizeof(cell), "%lu", procs[i].rss_kb);
                    else strlcpy(cell, "-", sizeof(cell));
                } else if (!strcmp(fn,"vsz")) {
                    if (procs[i].valid & OSIF_PROC_VALID_VSZ) snprintf(cell, sizeof(cell), "%lu", procs[i].vsz_kb);
                    else strlcpy(cell, "-", sizeof(cell));
                } else if (!strcmp(fn,"surface")) {
                    ps_surface_label(&procs[i], cell, sizeof(cell));
                } else if (!strcmp(fn,"seat")) {
                    snprintf(cell, sizeof(cell), "%s", procs[i].seat);
                } else if (!strcmp(fn,"start")||!strcmp(fn,"stime")||!strcmp(fn,"c")) {
                    strlcpy(cell, "-", sizeof(cell));
                }
                else if (!strcmp(fn,"%cpu")) {
                    double pct = 0;
                    if ((procs[i].valid & OSIF_PROC_VALID_CPU) && uptime_secs > 0 && hz > 0)
                        pct = ((double)procs[i].cpu_time / hz / uptime_secs) * 100.0;
                    if ((procs[i].valid & OSIF_PROC_VALID_CPU) && uptime_secs > 0 && hz > 0)
                        snprintf(cell, sizeof(cell), "%.1f", pct);
                    else strlcpy(cell, "-", sizeof(cell));
                } else if (!strcmp(fn,"%mem")) {
                    double pct = 0;
                    if ((procs[i].valid & OSIF_PROC_VALID_RSS) && total_mem_kb > 0)
                        pct = ((double)procs[i].rss_kb / total_mem_kb) * 100.0;
                    if ((procs[i].valid & OSIF_PROC_VALID_RSS) && total_mem_kb > 0)
                        snprintf(cell, sizeof(cell), "%.1f", pct);
                    else strlcpy(cell, "-", sizeof(cell));
                } else cell[0] = '\0';
                jash_writef(w, ctx, "%s%s", f ? " " : "", cell);
            }
            jash_writes(w, ctx, "\n");
        }
        return 0;
    }

    /* ── Generate output ──────────────────────────────────────────────────── */
    
    /* Dynamic buffer for large output */
    size_t output_cap = 65536;
    char* output = malloc(output_cap);
    if (!output) {
        jash_writes(w, ctx, "ps: out of memory\n");
        return 1;
    }
    output[0] = '\0';
    size_t output_len = 0;
    
    #define PS_APPEND(str) do { \
        size_t len = strlen(str); \
        if (output_len + len >= output_cap) { \
            output_cap *= 2; \
            char* new_out = realloc(output, output_cap); \
            if (!new_out) { free(output); jash_writes(w, ctx, "ps: OOM\n"); return 1; } \
            output = new_out; \
        } \
        memcpy(output + output_len, str, len); \
        output_len += len; \
        output[output_len] = '\0'; \
    } while(0)
    
    if (tree_mode) {
        /* Tree view */
        if (!piped) {
            PS_APPEND(PS_COLOR_BOLD PS_COLOR_CYAN);
        }
        char header[256];
        snprintf(header, sizeof(header), "Process Tree (UID %d) — %d processes\n",
                 (int)my_uid, nprocs);
        PS_APPEND(header);
        if (!piped) {
            PS_APPEND(PS_COLOR_RESET);
        }
        
        /* Write header first */
        jash_writes(w, ctx, output);
        output[0] = '\0';
        output_len = 0;
        
        /* Print tree */
        int is_last[PS_MAX_TREE_DEPTH + 1] = {0};
        
        /* Find root processes */
        for (int i = 0; i < nprocs; i++) {
            if (procs[i].ppid == 0 || procs[i].ppid == 1) {
                char line[512];
                char rss_kb[24];
                if (procs[i].valid & OSIF_PROC_VALID_RSS)
                    snprintf(rss_kb, sizeof(rss_kb), "%luKB", procs[i].rss_kb);
                else strlcpy(rss_kb, "-", sizeof(rss_kb));
                const char* color = piped ? "" : ps_state_color(procs[i].state);
                const char* icon = piped ? "" : ps_state_icon(procs[i].state);
                const char* reset = piped ? "" : PS_COLOR_RESET;
                
                if (piped) {
                    snprintf(line, sizeof(line), "%s PID %d %s %c %s\n",
                             icon, (int)procs[i].pid, procs[i].comm,
                             procs[i].state, rss_kb);
                } else {
                    snprintf(line, sizeof(line), "%s%s%s PID %d %s%s%s (%c) %s%s\n",
                             color, icon, reset,
                             (int)procs[i].pid, color, procs[i].comm, reset,
                             procs[i].state, rss_kb, reset);
                }
                jash_writes(w, ctx, line);
                
                ps_print_tree(procs, nprocs, procs[i].pid, 1, is_last, w, ctx, piped);
            }
        }
        
        free(output);
        return 0;
    }
    
    if (top_n > 0) {
        /* Top N view */
        if (!piped) {
            PS_APPEND(PS_COLOR_BOLD PS_COLOR_GREEN);
        }
        char header[256];
        snprintf(header, sizeof(header), "Top %d processes by CPU time:\n", top_n);
        PS_APPEND(header);
        if (!piped) {
            PS_APPEND(PS_COLOR_RESET);
        }
        
        ps_sort(procs, nprocs, ps_cmp_cpu, 1);
        
        /* Header line */
        if (!piped) {
            PS_APPEND(PS_COLOR_BOLD);
        }
        PS_APPEND("  PID USER      COMM                  CPU     RSS    STATE\n");
        if (!piped) {
            PS_APPEND(PS_COLOR_RESET);
        }
        
        for (int i = 0; i < top_n && i < nprocs; i++) {
            char line[512];
            const char* uname = ps_get_username(procs[i].uid);
            const char* color = piped ? "" : ps_state_color(procs[i].state);
            const char* icon = piped ? "" : ps_state_icon(procs[i].state);
            const char* reset = piped ? "" : PS_COLOR_RESET;
            
            char cpu_str[16], rss_str[16];
            if (procs[i].valid & OSIF_PROC_VALID_CPU)
                ps_format_time(procs[i].cpu_time, cpu_str, sizeof(cpu_str));
            else strlcpy(cpu_str, "-", sizeof(cpu_str));
            if (procs[i].valid & OSIF_PROC_VALID_RSS)
                ps_format_size(procs[i].rss_kb, rss_str, sizeof(rss_str));
            else strlcpy(rss_str, "-", sizeof(rss_str));
            
            /* Unicode-aware padding: icon + space + comm = 23 visual cols */
            char icon_comm[128];
            snprintf(icon_comm, sizeof(icon_comm), "%s %s", icon, procs[i].comm);
            char icon_comm_padded[160];
            ps_pad_visual(icon_comm_padded, sizeof(icon_comm_padded), icon_comm, 23);
            
            snprintf(line, sizeof(line), "%s%5d%s %-9s %s%s%s %s%-4s%s %6s %6s\n",
                     piped ? "" : PS_COLOR_CYAN, (int)procs[i].pid, reset,
                     uname,
                     color, reset,
                     icon_comm_padded,
                     color, ps_stat_string(procs[i].state, procs[i].foreground), reset,
                     rss_str, cpu_str);
            PS_APPEND(line);
        }
        
        PS_APPEND("\n");
        
        /* Top by memory */
        if (!piped) {
            PS_APPEND(PS_COLOR_BOLD PS_COLOR_YELLOW);
        }
        snprintf(header, sizeof(header), "Top %d processes by Memory:\n", top_n);
        PS_APPEND(header);
        if (!piped) {
            PS_APPEND(PS_COLOR_RESET);
        }
        
        ps_sort(procs, nprocs, ps_cmp_mem, 1);
        
        if (!piped) {
            PS_APPEND(PS_COLOR_BOLD);
        }
        PS_APPEND("  PID USER      COMM                  RSS     CPU    STATE\n");
        if (!piped) {
            PS_APPEND(PS_COLOR_RESET);
        }
        
        for (int i = 0; i < top_n && i < nprocs; i++) {
            char line[512];
            const char* uname = ps_get_username(procs[i].uid);
            const char* color = piped ? "" : ps_state_color(procs[i].state);
            const char* icon = piped ? "" : ps_state_icon(procs[i].state);
            const char* reset = piped ? "" : PS_COLOR_RESET;
            
            char cpu_str[16], rss_str[16];
            if (procs[i].valid & OSIF_PROC_VALID_CPU)
                ps_format_time(procs[i].cpu_time, cpu_str, sizeof(cpu_str));
            else strlcpy(cpu_str, "-", sizeof(cpu_str));
            if (procs[i].valid & OSIF_PROC_VALID_RSS)
                ps_format_size(procs[i].rss_kb, rss_str, sizeof(rss_str));
            else strlcpy(rss_str, "-", sizeof(rss_str));
            
            /* Unicode-aware padding: icon + space + comm = 23 visual cols */
            char icon_comm[128];
            snprintf(icon_comm, sizeof(icon_comm), "%s %s", icon, procs[i].comm);
            char icon_comm_padded[160];
            ps_pad_visual(icon_comm_padded, sizeof(icon_comm_padded), icon_comm, 23);
            
            snprintf(line, sizeof(line), "%s%5d%s %-9s %s%s%s %s%-4s%s %6s %6s\n",
                     piped ? "" : PS_COLOR_CYAN, (int)procs[i].pid, reset,
                     uname,
                     color, reset,
                     icon_comm_padded,
                     color, ps_stat_string(procs[i].state, procs[i].foreground), reset,
                     rss_str, cpu_str);
            PS_APPEND(line);
        }
        
    } else if (show_all || show_full) {
        /* ps aux / ps -ef style */
        if (!piped) {
            PS_APPEND(PS_COLOR_BOLD PS_COLOR_WHITE);
        }
        
        if (show_all) {
            PS_APPEND("USER         PID %CPU %MEM    VSZ   RSS SURFACE  STAT START   TIME COMM\n");
        } else {
            PS_APPEND("UID          PID    PPID  C STIME SURFACE    TIME COMM\n");
        }
        
        if (!piped) {
            PS_APPEND(PS_COLOR_RESET);
        }
        
        
        for (int i = 0; i < nprocs; i++) {
            char line[512];
            char surface_buf[24];
            const char* uname = ps_get_username(procs[i].uid);
            const char* color = piped ? "" : ps_state_color(procs[i].state);
            const char* icon = piped ? "" : ps_state_icon(procs[i].state);
            const char* reset = piped ? "" : PS_COLOR_RESET;
            ps_surface_label(&procs[i], surface_buf, sizeof(surface_buf));
            char rss_buf[16], vsz_buf[16], cpu_buf[16], mem_buf[16];
            char start_buf[8], c_buf[8];

            if (procs[i].valid & OSIF_PROC_VALID_RSS)
                snprintf(rss_buf, sizeof(rss_buf), "%lu", procs[i].rss_kb);
            else strlcpy(rss_buf, "-", sizeof(rss_buf));
            if (procs[i].valid & OSIF_PROC_VALID_VSZ)
                snprintf(vsz_buf, sizeof(vsz_buf), "%lu", procs[i].vsz_kb);
            else strlcpy(vsz_buf, "-", sizeof(vsz_buf));
            if ((procs[i].valid & OSIF_PROC_VALID_CPU) && uptime_secs > 0 && hz > 0)
                snprintf(cpu_buf, sizeof(cpu_buf), "%.1f",
                         ((double)procs[i].cpu_time / hz / uptime_secs) * 100.0);
            else strlcpy(cpu_buf, "-", sizeof(cpu_buf));
            if ((procs[i].valid & OSIF_PROC_VALID_RSS) && total_mem_kb > 0)
                snprintf(mem_buf, sizeof(mem_buf), "%.1f",
                         ((double)procs[i].rss_kb / total_mem_kb) * 100.0);
            else strlcpy(mem_buf, "-", sizeof(mem_buf));
            /* Proc-NU has no wall-clock START and no nice/C observation. */
            strlcpy(start_buf, "-", sizeof(start_buf));
            strlcpy(c_buf, "-", sizeof(c_buf));
            
            if (show_all) {
                snprintf(line, sizeof(line), 
                        "%-9s %s%6d%s %4s %4s %6s %6s %-10s %s%-4s%s %5s %8s %s%s%s %s\n",
                        uname,
                        piped ? "" : PS_COLOR_CYAN, (int)procs[i].pid, reset,
                        cpu_buf, mem_buf, vsz_buf, rss_buf,
                        surface_buf,
                        color, ps_stat_string(procs[i].state, procs[i].foreground), reset,
                        start_buf, "-",
                        color, icon, reset,
                        procs[i].comm);
            } else {
                /* ps -ef style */
                snprintf(line, sizeof(line),
                        "%-9s %s%6d%s %6d  %s %-5s %-10s %8s %s%s%s %s\n",
                        uname,
                        piped ? "" : PS_COLOR_CYAN, (int)procs[i].pid, reset,
                        (int)procs[i].ppid,
                        c_buf, start_buf, surface_buf, "-",
                        color, icon, reset,
                        procs[i].comm);
            }
            PS_APPEND(line);
        }
        
    } else {
        /* Default: simple list */
        if (!piped) {
            PS_APPEND(PS_COLOR_BOLD PS_COLOR_WHITE);
        }
        PS_APPEND("  PID USER      COMM                  STATE    RSS    CPU\n");
        if (!piped) {
            PS_APPEND(PS_COLOR_RESET);
        }
        
        for (int i = 0; i < nprocs; i++) {
            char line[512];
            const char* uname = ps_get_username(procs[i].uid);
            const char* color = piped ? "" : ps_state_color(procs[i].state);
            const char* icon = piped ? "" : ps_state_icon(procs[i].state);
            const char* reset = piped ? "" : PS_COLOR_RESET;
            
            char cpu_str[16], rss_str[16];
            if (procs[i].valid & OSIF_PROC_VALID_CPU)
                ps_format_time(procs[i].cpu_time, cpu_str, sizeof(cpu_str));
            else strlcpy(cpu_str, "-", sizeof(cpu_str));
            if (procs[i].valid & OSIF_PROC_VALID_RSS)
                ps_format_size(procs[i].rss_kb, rss_str, sizeof(rss_str));
            else strlcpy(rss_str, "-", sizeof(rss_str));
            
            /* Unicode-aware padding: icon + space + comm = 23 visual cols */
            char icon_comm[128];
            snprintf(icon_comm, sizeof(icon_comm), "%s %s", icon, procs[i].comm);
            char icon_comm_padded[160];
            ps_pad_visual(icon_comm_padded, sizeof(icon_comm_padded), icon_comm, 23);
            
            snprintf(line, sizeof(line), "%s%5d%s %-9s %s%s%s %s%-4s%s %6s %6s\n",
                     piped ? "" : PS_COLOR_CYAN, (int)procs[i].pid, reset,
                     uname,
                     color, reset,
                     icon_comm_padded,
                     color, ps_stat_string(procs[i].state, procs[i].foreground), reset,
                     rss_str, cpu_str);
            PS_APPEND(line);
        }
    }
    
    /* Update cache */
    if (use_cache) {
        free(g_ps_cache);
        g_ps_cache = strdup(output);
        g_ps_cache_time = now;
    }
    
    /* Write output */
    jash_writes(w, ctx, output);
    
    free(output);
    return 0;
}
