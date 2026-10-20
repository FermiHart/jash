// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — ui/statusbar — Living Dashboard with 7 modes
//
// Modes: Computer, Dev, AI, Cognitive, Focus, Personal, Debug
// Auto-switching based on context (git repo → Dev, AI on → AI, slm-mesh loaded → Cognitive)

#include "statusbar.h"
#include <theme/theme.h>
#include <core/env.h>
#include <core/jash_ai.h>
#include <core/llm_status.h>
#include <module/sigil.h>
#include <module/loader.h>
#include <osif/osinfo.h>
#include <sys/syscalls.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#ifdef JASH_TINALINUX
#include <osif/tina/surface.h>
#endif
#include <sys/select.h>
#include <sys/statvfs.h>

static jash_statusbar_t g_sb = {0};
static uint64_t g_session_start_ns = 0;
static int g_terminal_rows = 24;
static int g_terminal_cols = 80;
static int g_scroll_region_active = 0;

#define NS_PER_SEC 1000000000ULL
#define NET_REFRESH_NS      (2ULL * NS_PER_SEC)
#define BATTERY_REFRESH_NS  (30ULL * NS_PER_SEC)
#define GIT_REFRESH_NS      (2ULL * NS_PER_SEC)
#define LLM_STATUS_REFRESH_NS (1ULL * NS_PER_SEC)
#define SYS_REFRESH_NS      (2ULL * NS_PER_SEC)   /* mem/disk probes */
#define PROJ_REFRESH_NS     (2ULL * NS_PER_SEC)   /* project-type stat()s */
#define WIN_REFRESH_NS      (1ULL * NS_PER_SEC)   /* TIOCGWINSZ re-probe */

/* Mode names */
static const char* MODE_NAMES[] = {
    "computer", "dev", "ai", "cognitive", "focus", "personal", "debug"
};

/* Per-mode accent (xterm-256 index) for the full-width badge. */
static const unsigned char MODE_ACCENT[SB_MODE_COUNT] = {
    75,   /* computer  — steel blue   */
    176,  /* dev       — orchid       */
    87,   /* ai        — cyan         */
    123,  /* cognitive — teal         */
    114,  /* focus     — sage green   */
    221,  /* personal  — warm amber   */
    203   /* debug     — coral        */
};

/* Bar canvas background.  Every SGR reset emitted INSIDE the bar must be
 * immediately followed by SB_BG so the ink runs edge to edge. SB_RST
 * resets fg/bold but deliberately KEEPS the bar background. */
#define SB_BG   "\033[48;5;235m"
#define SB_RST  "\033[39;22m"

/* ─── ANSI-aware segment compositor ─────────────────────────────────────
 * The bar is composed off-screen into segments that track BYTES and
 * VISIBLE COLUMNS separately: SGR escape sequences cost 0 columns, UTF-8
 * continuation bytes cost 0 columns, every glyph costs exactly 1 (the bar
 * only emits single-cell glyphs — no emoji). This is what lets the bar
 * fill the terminal edge to edge with an exact right-aligned zone. */
struct sb_seg {
    char     buf[1024];
    uint32_t len;       /* bytes used */
    uint32_t cols;      /* visible columns used */
    uint32_t max_cols;  /* clip limit (visible columns) */
    int      clipped;
};

static void seg_init(sb_seg_t* g, uint32_t max_cols) {
    g->len = 0;
    g->cols = 0;
    g->max_cols = max_cols;
    g->clipped = 0;
    g->buf[0] = '\0';
}

static void seg_str(sb_seg_t* g, const char* s) {
    if (!s) return;
    while (*s) {
        unsigned char c = (unsigned char)*s;
        if (c == 0x1B) {
            /* Copy a whole escape sequence — zero columns. Always copied
             * even past the clip point so SGR state stays consistent. */
            const char* e = s + 1;
            if (*e == '[') {
                e++;
                while (*e && !((unsigned char)*e >= 0x40 && (unsigned char)*e <= 0x7E)) e++;
                if (*e) e++;
            } else if (*e) {
                e++;
            }
            uint32_t n = (uint32_t)(e - s);
            if (g->len + n + 1 >= sizeof(g->buf)) return;
            for (uint32_t i = 0; i < n; i++) g->buf[g->len++] = s[i];
            g->buf[g->len] = '\0';
            s = e;
            continue;
        }
        /* One UTF-8 unit = one column (single-cell glyphs only). */
        uint32_t n = 1;
        if ((c & 0xE0) == 0xC0) n = 2;
        else if ((c & 0xF0) == 0xE0) n = 3;
        else if ((c & 0xF8) == 0xF0) n = 4;
        uint32_t avail = 0;
        while (avail < n && s[avail]) avail++;
        if (g->cols + 1 > g->max_cols) { g->clipped = 1; s += avail; continue; }
        if (g->len + avail + 1 >= sizeof(g->buf)) { g->clipped = 1; return; }
        for (uint32_t i = 0; i < avail; i++) g->buf[g->len++] = s[i];
        g->buf[g->len] = '\0';
        g->cols++;
        s += avail;
    }
}

static void seg_int(sb_seg_t* g, int val) {
    char tmp[16];
    int ti = 0;
    if (val == 0) tmp[ti++] = '0';
    else {
        int v = val < 0 ? -val : val;
        while (v > 0 && ti < 12) { tmp[ti++] = (char)('0' + v % 10); v /= 10; }
        if (val < 0 && ti < 15) tmp[ti++] = '-';
    }
    char out[16];
    int o = 0;
    while (ti > 0) out[o++] = tmp[--ti];
    out[o] = '\0';
    seg_str(g, out);
}

static void seg_branch(sb_seg_t* g, const char* branch) {
    if (!branch) return;
    uint32_t len = 0;
    while (branch[len]) len++;
    uint32_t max = (len <= 16) ? len : 13;
    char out[24];
    uint32_t o = 0;
    for (uint32_t i = 0; i < max; i++) {
        unsigned char c = (unsigned char)branch[i];
        if (c == 0x1B || c < 0x20 || c == 0x7F) continue;
        out[o++] = (char)c;
    }
    out[o] = '\0';
    seg_str(g, out);
    if (len > 16) seg_str(g, "…");
}

static void seg_bar(sb_seg_t* g, int percentage, const char* color) {
    enum { BLOCKS = 6 };
    if (percentage < 0) percentage = 0;
    if (percentage > 100) percentage = 100;
    int filled = (percentage * BLOCKS) / 100;
    seg_str(g, color);
    for (int i = 0; i < BLOCKS; i++) seg_str(g, i < filled ? "█" : "░");
    seg_str(g, SB_RST);
}

static void get_terminal_size(void) {
#ifdef JASH_TINALINUX
    if (jash_tina_surface_native_output()) {
        unsigned columns = 0, rows = 0;
        if (jash_tina_surface_viewport(&columns, &rows) == 0) {
            g_terminal_cols = (int)columns;
            g_terminal_rows = (int)rows;
        }
        return;
    }
#endif
    struct winsize w;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_row > 0) {
        g_terminal_rows = w.ws_row;
        g_terminal_cols = w.ws_col;
    }
}

/* ─── System info (REAL numbers — OSIF L1 + statvfs) ────────────────── */

static int get_memory_usage(void) {
    osif_meminfo_t mi;
    if (bear_osinfo_meminfo(&mi) == 0 && mi.total_kb > 0) {
        unsigned long used = mi.total_kb > mi.available_kb
                           ? mi.total_kb - mi.available_kb : 0;
        return (int)((used * 100UL) / mi.total_kb);
    }
    return -1;  /* unknown — rendered as "--" */
}

static int get_disk_usage(void) {
    struct statvfs vfs;
    if (statvfs("/", &vfs) == 0 && vfs.f_blocks > 0) {
        unsigned long long total = (unsigned long long)vfs.f_blocks;
        unsigned long long avail = (unsigned long long)vfs.f_bavail;
        return (int)(((total - avail) * 100ULL) / total);
    }
    return -1;  /* unknown — rendered as "--" */
}

/* ─── Custom section registration (for modules) ──────────────────────── */

static jash_statusbar_section_fn g_custom_sections[JASH_STATUSBAR_MAX_SECTIONS];
static int g_custom_section_count = 0;

int jash_statusbar_register_section(jash_statusbar_section_fn fn) {
    if (!fn) return -1;
    if (g_custom_section_count >= JASH_STATUSBAR_MAX_SECTIONS) return -1;
    g_custom_sections[g_custom_section_count] = fn;
    return g_custom_section_count++;
}

void jash_statusbar_unregister_section(int id) {
    if (id < 0 || id >= g_custom_section_count) return;
    g_custom_sections[id] = NULL;
    // Compact array
    int dst = 0;
    for (int src = 0; src < g_custom_section_count; src++) {
        if (g_custom_sections[src]) {
            g_custom_sections[dst++] = g_custom_sections[src];
        }
    }
    g_custom_section_count = dst;
}

/* ─── Terminal setup/teardown ───────────────────────────────────────── */

void jash_statusbar_teardown_terminal(void) {
    if (g_scroll_region_active) {
        /* Restore scroll region to full terminal */
        WRITE_IGNORE(STDOUT_FILENO, "\033[r", 3);
        /* Move to top status line and clear it */
        char buf[64];
        int n = snprintf(buf, sizeof(buf), "\033[1;1H\033[2K");
        WRITE_IGNORE(STDOUT_FILENO, buf, n);
        g_scroll_region_active = 0;
    }
}

/* Volatile flags set by signal handlers; polled by the main loop. */
static volatile sig_atomic_t g_sigwinch_pending = 0;
static volatile sig_atomic_t g_sigterm_pending = 0;

#ifdef SIGWINCH
static void sigwinch_handler(int sig) {
    (void)sig;
    g_sigwinch_pending = 1;
}
#endif

static void exit_handler(void) {
    jash_statusbar_teardown_terminal();
}

/* SECURITY (BEARJASH-002): Async-signal-safe handler.
 * Removed signal() call — not safe inside a signal handler.
 * Reset behavior handled via SA_RESETHAND in sigaction setup.
 * F E R M I ∞ H A R T */
static void signal_handler(int sig) {
    g_sigterm_pending = 1;
}

int jash_statusbar_sigwinch_pending(void) { return g_sigwinch_pending; }
int jash_statusbar_sigterm_pending(void) { return g_sigterm_pending; }
void jash_statusbar_clear_sigwinch(void) { g_sigwinch_pending = 0; }
void jash_statusbar_clear_sigterm(void) { g_sigterm_pending = 0; }

void jash_statusbar_setup_terminal(void) {
    get_terminal_size();
    if (g_terminal_rows < 5) return;  /* Terminal too small */
    
    /* Configure scroll region: line 1 is sovereign statusbar, commands scroll below. */
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "\033[2;%dr\033[2;1H", g_terminal_rows);
    WRITE_IGNORE(STDOUT_FILENO, buf, n);
    g_scroll_region_active = 1;
    
    /* SECURITY (BEARJASH-002): Use sigaction for all signal setup.
     * SA_RESETHAND handles the "reset to default after delivery" logic
     * safely, without calling signal() inside the handler.
     * F E R M I ∞ H A R T */
    struct sigaction sa_term;
    sa_term.sa_handler = signal_handler;
    sigemptyset(&sa_term.sa_mask);
    sa_term.sa_flags = SA_RESETHAND | SA_RESTART; /* Reset after delivery */
    sigaction(SIGTERM, &sa_term, NULL);
    sigaction(SIGHUP, &sa_term, NULL);

    atexit(exit_handler);
    
    /* Register SIGWINCH handler for terminal resize */
#ifdef SIGWINCH
    struct sigaction sa;
    sa.sa_handler = sigwinch_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGWINCH, &sa, NULL);
#endif
}

/* ─── Mode control ──────────────────────────────────────────────────── */

void jash_statusbar_set_mode(jash_sb_mode_t mode) {
    if (mode >= 0 && mode < SB_MODE_COUNT) {
        g_sb.mode = mode;
        /* An explicit choice PINS the mode — otherwise auto_detect_mode
         * steals it back on the next tick and the user sees the bar
         * "fighting" their selection. `statusbar auto on` re-enables. */
        g_sb.auto_switch = 0;
        g_sb.dirty = 1;
    }
}

jash_sb_mode_t jash_statusbar_get_mode(void) {
    return g_sb.mode;
}

void jash_statusbar_toggle_mode(void) {
    g_sb.mode = (g_sb.mode + 1) % SB_MODE_COUNT;
    g_sb.auto_switch = 0;   /* cycling is an explicit choice — pin it */
    g_sb.dirty = 1;
}

void jash_statusbar_set_auto_switch(int on) {
    g_sb.auto_switch = on;
}

const char* jash_statusbar_mode_name(jash_sb_mode_t mode) {
    if (mode >= 0 && mode < SB_MODE_COUNT) return MODE_NAMES[mode];
    return "unknown";
}

/* Auto-detect best mode based on context */
static void auto_detect_mode(jash_state_t* s) {
    if (!g_sb.auto_switch) return;
    
    /* AI mode active → AI statusbar */
    if (jash_ai_active()) {
        if (g_sb.mode != SB_MODE_AI) {
            g_sb.mode = SB_MODE_AI;
            g_sb.dirty = 1;
        }
        return;
    }
    
    /* SLM mesh or primitives loaded → Cognitive statusbar (ADR-0022) */
    if (jmod_find("slm-mesh") || jmod_find("slm-primitives")) {
        if (g_sb.mode != SB_MODE_COGNITIVE) {
            g_sb.mode = SB_MODE_COGNITIVE;
            g_sb.dirty = 1;
        }
        return;
    }
    
    /* In git repo → Dev mode */
    if (g_sb.cached_has_git) {
        if (g_sb.mode != SB_MODE_DEV) {
            g_sb.mode = SB_MODE_DEV;
            g_sb.dirty = 1;
        }
        return;
    }
    
    /* Default → Computer mode */
    if (g_sb.mode != SB_MODE_COMPUTER) {
        g_sb.mode = SB_MODE_COMPUTER;
        g_sb.dirty = 1;
    }
}

/* ─── Status bar initialization ─────────────────────────────────────── */

void jash_statusbar_init(jash_state_t* s) {
    if (g_session_start_ns == 0) {
        struct timespec ts;
        if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
            g_session_start_ns = (uint64_t)ts.tv_sec * 1000000000ULL +
                                 (uint64_t)ts.tv_nsec;
    }

    /* Statusbar is OFF by default (still being optimized — F E R M I ∞ H A R T).
     * Opt in explicitly via JASH_STATUSBAR=1 / on, or `statusbar on` at runtime. */
    const char* env = jash_env_get(s, "JASH_STATUSBAR");
    if (env && (env[0] == '1' || (env[0] == 'o' && env[1] == 'n'))) {
        g_sb.enabled = 1;
    } else {
        g_sb.enabled = 0;
    }
    
    /* Check for mode override */
    const char* mode_env = jash_env_get(s, "JASH_STATUSBAR_MODE");
    if (mode_env) {
        for (int i = 0; i < SB_MODE_COUNT; i++) {
            if (strcmp(mode_env, MODE_NAMES[i]) == 0) {
                g_sb.mode = (jash_sb_mode_t)i;
                g_sb.auto_switch = 0;
                break;
            }
        }
    } else {
        g_sb.mode = SB_MODE_COMPUTER;
        g_sb.auto_switch = 1;
    }
    
    g_sb.dirty = 1;
    g_sb.rendered = 0;
}

int jash_statusbar_enabled(const jash_state_t* s) {
    (void)s;
    return g_sb.enabled;
}

int jash_statusbar_region_active(void) {
    return g_scroll_region_active;
}

void jash_statusbar_set_enabled(jash_state_t* s, int enabled) {
    if (g_sb.enabled == enabled) return;

    g_sb.enabled = enabled;
    g_sb.dirty = 1;
    if (enabled) {
        jash_env_set(s, "JASH_STATUSBAR", "1");
        jash_statusbar_setup_terminal();
    } else {
        jash_env_set(s, "JASH_STATUSBAR", "0");
        jash_statusbar_teardown_terminal();
        /* Hide AI mode indicator when statusbar is disabled */
        render_ai_mode_indicator();
    }
}

void jash_statusbar_mark_dirty(void) {
    g_sb.dirty = 1;
}

/* ─── Status bar rendering ──────────────────────────────────────────── */

/* ─── Network I/O ───────────────────────────────────────────────────── */

static uint64_t last_rx_bytes = 0;
static uint64_t last_tx_bytes = 0;
static struct timespec last_net_time;
static double rx_rate = 0.0;  /* bytes per second */
static double tx_rate = 0.0;
static int net_initialized = 0;

static void update_network_io(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return;
    
    /* Initialize on first call */
    if (!net_initialized) {
        last_net_time = now;
        net_initialized = 1;
        return;
    }

    /* ADR-0017: use OSIF L1 — no popen, no cat|grep|awk. */
    osif_netstat_t ns = {0, 0};
    if (bear_osinfo_netstat(&ns) == 0) {
        uint64_t rx = ns.rx_bytes, tx = ns.tx_bytes;
        if (last_rx_bytes > 0 && last_tx_bytes > 0) {
            double elapsed = (now.tv_sec - last_net_time.tv_sec) +
                            (now.tv_nsec - last_net_time.tv_nsec) / 1e9;
            if (elapsed > 0.1) {
                rx_rate = (rx - last_rx_bytes) / elapsed;
                tx_rate = (tx - last_tx_bytes) / elapsed;
                if (rx_rate < 0) rx_rate = 0;
                if (tx_rate < 0) tx_rate = 0;
                if (rx_rate > 1e9) rx_rate = 1e9;
                if (tx_rate > 1e9) tx_rate = 1e9;
            }
        }
        last_rx_bytes = rx;
        last_tx_bytes = tx;
        last_net_time = now;
    }
}

static void seg_bytes(sb_seg_t* g, double bytes) {
    if (bytes < 1024) {
        seg_int(g, (int)bytes);
        seg_str(g, "B/s");
    } else if (bytes < 1024 * 1024) {
        seg_int(g, (int)(bytes / 1024));
        seg_str(g, "KB/s");
    } else {
        seg_int(g, (int)(bytes / (1024 * 1024)));
        seg_str(g, "MB/s");
    }
}

/* ─── Battery Status ────────────────────────────────────────────────── */

static int battery_percent = -1;  /* -1 = no battery */
static int battery_charging = 0;

static void update_battery(void) {
#ifdef BEAR_OS_NETBSD
    /* BeyondBSD: no popen/stdio for battery stats yet */
    battery_percent = -1;  /* no battery */
    battery_charging = 0;
#else
    #ifdef __APPLE__
    /* macOS */
    FILE* fp = popen("pmset -g batt 2>/dev/null | grep -Eo '\\d+%' | head -1", "r");
    if (fp) {
        int percent = -1;
        if (fscanf(fp, "%d%%", &percent) == 1) {
            battery_percent = percent;
        }
        pclose(fp);
        
        /* Check if charging */
        fp = popen("pmset -g batt 2>/dev/null | grep -q 'AC Power' && echo 1 || echo 0", "r");
        if (fp) {
            int charging = 0;
            if (fscanf(fp, "%d", &charging) == 1) {
                battery_charging = charging;
            }
            pclose(fp);
        }
    }
    #else
    /* Linux */
    FILE* fp = popen("cat /sys/class/power_supply/BAT0/capacity 2>/dev/null", "r");
    if (fp) {
        int percent = -1;
        if (fscanf(fp, "%d", &percent) == 1) {
            battery_percent = percent;
        }
        pclose(fp);
        
        fp = popen("cat /sys/class/power_supply/BAT0/status 2>/dev/null", "r");
        if (fp) {
            char status[32] = {0};
            if (fscanf(fp, "%31s", status) == 1) {
                battery_charging = (strcmp(status, "Charging") == 0);
            }
            pclose(fp);
        }
    }
    #endif
#endif
}



/* ─── Mode Indicator ────────────────────────────────────────────────── */

/* ─── Git info ──────────────────────────────────────────────────────── */

/* Strip control / escape bytes from a string in place so it is safe for
 * terminal output. Keeps TAB, LF, CR and printable ASCII intact. */
static void sanitize_term(char* s) {
    unsigned j = 0;
    for (unsigned i = 0; s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        int keep = (c >= 0x20 && c != 0x7F && c < 0x80) ||
                   c == '\t' || c == '\n' || c == '\r';
        if (keep) s[j++] = s[i];
    }
    s[j] = '\0';
}

/* Run a command with arguments via fork+execvp (no shell) and capture
 * stdout into `out` (cap bytes). Returns bytes read or -1 on error. */
static int fork_exec_capture(char* out, unsigned cap, const char* cwd,
                             const char* cmd, char* const argv[]) {
    int pipefd[2];
    if (pipe(pipefd) != 0) return -1;

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    if (pid == 0) {
        /* Child */
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        /* Redirect stderr to /dev/null to suppress git errors (e.g. "dubious ownership") */
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        if (cwd) { int _cr = chdir(cwd); (void)_cr; }
        /* Restore default signal handlers for exec'd process */
        signal(SIGPIPE, SIG_DFL);
        execvp(cmd, argv);
        _exit(127);
    }

    /* Parent */
    close(pipefd[1]);
    unsigned total = 0;
    for (;;) {
        if (total + 1 >= cap) break;
        ssize_t n = read(pipefd[0], out + total, cap - total - 1);
        if (n <= 0) break;
        total += (unsigned)n;
    }
    out[total] = '\0';
    close(pipefd[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return -1;
    return (int)total;
}

static int get_git_branch(char* out, unsigned cap, const char* cwd) {
    /* Quick check: does .git/HEAD exist? (avoids spawning git outside repos) */
    char git_head[512];
    unsigned i = 0;
    while (cwd && cwd[i] && i + 1 < sizeof(git_head)) { git_head[i] = cwd[i]; i++; }
    const char* suffix = "/.git/HEAD";
    for (unsigned k = 0; suffix[k] && i + 1 < sizeof(git_head); k++) git_head[i++] = suffix[k];
    git_head[i] = '\0';
    if (access(git_head, F_OK) != 0) return 0;  /* not a git repo */

    char buf[256];
    char* argv[] = {"git", "rev-parse", "--abbrev-ref", "HEAD", NULL};
    int n = fork_exec_capture(buf, sizeof(buf), cwd, "git", argv);
    if (n <= 0) return 0;
    if (n > 0 && buf[n-1] == '\n') buf[n-1] = '\0';
    sanitize_term(buf);
    if (strcmp(buf, "HEAD") == 0) return 0;
    unsigned j = 0;
    while (buf[j] && j + 1 < cap) { out[j] = buf[j]; j++; }
    out[j] = '\0';
    return 1;
}

static int get_git_status(const char* cwd) {
    /* Quick check: does .git/HEAD exist? (avoids spawning git outside repos) */
    char git_head[512];
    unsigned i = 0;
    while (cwd && cwd[i] && i + 1 < sizeof(git_head)) { git_head[i] = cwd[i]; i++; }
    const char* suffix = "/.git/HEAD";
    for (unsigned k = 0; suffix[k] && i + 1 < sizeof(git_head); k++) git_head[i++] = suffix[k];
    git_head[i] = '\0';
    if (access(git_head, F_OK) != 0) return 0;  /* not a git repo */

    char buf[4096];
    char* argv[] = {"git", "status", "--porcelain", NULL};
    int n = fork_exec_capture(buf, sizeof(buf), cwd, "git", argv);
    if (n <= 0) return 0;

    int has_changes = 0;
    int has_untracked = 0;
    const char* p = buf;
    while (*p) {
        if (p[0] == '?' && p[1] == '?') has_untracked = 1;
        else has_changes = 1;
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }
    if (has_changes) return 1;
    if (has_untracked) return 2;
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════
 * FULL-WIDTH RENDER ENGINE
 *
 * Three pillars, each one killing a class of bug:
 *
 *  1. CURSOR DISCIPLINE — the frame is wrapped in DECSC/DECRC
 *     (\0337 … \0338). The cursor NEVER stays on the status line, so a
 *     prompt repaint can never land on row 1. This is the flicker fix:
 *     before, the 500 ms tick left the real cursor at 1;1 and the next
 *     prompt redraw drew the prompt INTO the bar.
 *
 *  2. DOUBLE BUFFER — the frame is composed off-screen and diffed against
 *     the previously written frame. Identical frame = zero bytes written
 *     to the tty. The bar only costs I/O when a glyph actually changed.
 *
 *  3. EDGE-TO-EDGE CANVAS — left zone + computed filler + right zone,
 *     using visible-column accounting (SGR = 0 cols), so every theme owns
 *     the full terminal width. The right zone always carries the AI MODE
 *     pill: white when OFF, red when ON.
 * ═══════════════════════════════════════════════════════════════════════ */

#define SB_MUT "\033[38;5;245m"

static void seg_mode_badge(sb_seg_t* g, jash_sb_mode_t mode) {
    char esc[24];
    snprintf(esc, sizeof(esc), "\033[1;30;48;5;%um", (unsigned)MODE_ACCENT[mode]);
    seg_str(g, esc);
    seg_str(g, " ");
    const char* mn = MODE_NAMES[mode];
    char up[16];
    int ui = 0;
    for (; mn[ui] && ui < 15; ui++)
        up[ui] = (char)(mn[ui] >= 'a' && mn[ui] <= 'z' ? mn[ui] - 32 : mn[ui]);
    up[ui] = '\0';
    seg_str(g, up);
    seg_str(g, " \033[0m" SB_BG);
}

static void seg_uptime(sb_seg_t* g, int uptime_sec) {
    seg_str(g, SB_MUT "up " SB_RST);
    if (uptime_sec < 3600) {
        seg_int(g, uptime_sec / 60);
        seg_str(g, "m");
    } else {
        seg_int(g, uptime_sec / 3600);
        seg_str(g, "h");
        seg_int(g, (uptime_sec % 3600) / 60);
        seg_str(g, "m");
    }
}

static void seg_battery(sb_seg_t* g) {
    if (battery_percent < 0) return;
    seg_str(g, SB_MUT " │ bat " SB_RST);
    seg_str(g, battery_charging ? "\033[93m↯" : "\033[92m■");
    seg_int(g, battery_percent);
    seg_str(g, "%" SB_RST);
}

void jash_statusbar_render(jash_writer_t w, void* ctx, const jash_state_t* s) {
    if (!g_sb.enabled) return;
    if (!w || !s) return;
    if (!isatty(STDOUT_FILENO)) return;
    if (!g_scroll_region_active) return;

    const jash_theme_t* theme = jash_theme_current();

    /* Auto-detect mode based on context */
    auto_detect_mode((jash_state_t*)s);

    /* Get current time */
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return;
    uint64_t now_ns = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
    int uptime_sec = (int)((now_ns - g_session_start_ns) / NS_PER_SEC);

    /* Terminal size: one ioctl per render was per-keystroke waste. Rate-limit
     * the re-probe; a pending SIGWINCH forces an immediate refresh. */
    static uint64_t last_win_probe_ns = 0;
    if (g_sigwinch_pending || now_ns - last_win_probe_ns >= WIN_REFRESH_NS) {
        get_terminal_size();
        last_win_probe_ns = now_ns;
    }
    uint32_t width = g_terminal_cols > 0 ? (uint32_t)g_terminal_cols : 80;

    /* Wall clock for the right-zone segments */
    time_t now = time(NULL);
    struct tm tm_info;
    localtime_r(&now, &tm_info);
    char time_str[32];
    strftime(time_str, sizeof(time_str), "%H:%M", &tm_info);

    char date_str[32];
    strftime(date_str, sizeof(date_str), "%a %d %b", &tm_info);

    /* Rate-limited probes */
    if (now_ns - g_sb.last_net_probe_ns >= NET_REFRESH_NS) {
        update_network_io();
        g_sb.last_net_probe_ns = now_ns;
    }
    if (now_ns - g_sb.last_battery_probe_ns >= BATTERY_REFRESH_NS) {
        update_battery();
        g_sb.last_battery_probe_ns = now_ns;
    }
    
    /* LLM status probe — refresh every second for cognitive mode */
    static uint64_t last_llm_probe_ns = 0;
    if (now_ns - last_llm_probe_ns >= LLM_STATUS_REFRESH_NS) {
        // LLM status is updated by llm_status.c, we just read it
        last_llm_probe_ns = now_ns;
    }

    /* Git info — refresh on first render or when the cache expired.
     * (The old condition was inverted and re-spawned `git status` on
     * EVERY 500 ms tick once a branch was cached.) */
    if (g_sb.last_git_probe_ns == 0 ||
        now_ns - g_sb.last_git_probe_ns >= GIT_REFRESH_NS) {
        g_sb.cached_has_git = get_git_branch(g_sb.cached_git_branch, sizeof(g_sb.cached_git_branch), s->cwd);
        g_sb.cached_git_status = g_sb.cached_has_git ? get_git_status(s->cwd) : 0;
        g_sb.last_git_probe_ns = now_ns;
    }

    /* ═══ RIGHT ZONE — exit status · AI MODE pill · clock ═══
     * The AI MODE pill is contractual: white when OFF, red when ON. */
    sb_seg_t right;
    seg_init(&right, width);
    if (s->last_status.code != 0) {
        seg_str(&right, theme->role_err);
        seg_str(&right, "✗");
        seg_int(&right, s->last_status.code);
        seg_str(&right, SB_RST " ");
    }
    if (jash_ai_active())
        seg_str(&right, "\033[1;97;48;5;124m AI MODE ON \033[0m" SB_BG);
    else
        seg_str(&right, "\033[1;97m AI MODE OFF" SB_RST);
    seg_str(&right, " \033[1m");
    seg_str(&right, time_str);
    seg_str(&right, SB_RST " ");

    /* ═══ LEFT ZONE — mode badge + mode content ═══ */
    sb_seg_t left;
    uint32_t left_max = width > right.cols + 1 ? width - right.cols - 1 : 0;
    seg_init(&left, left_max);
    seg_mode_badge(&left, g_sb.mode);
    seg_str(&left, " ");

    switch (g_sb.mode) {

    case SB_MODE_COMPUTER: {
        /* RAM/disk probes (procfs + statvfs) are rate-limited like the net
         * probe — the double buffer dedups the write, not the probes. */
        static int cached_ram = -1, cached_disk = -1;
        static uint64_t last_sys_probe_ns = 0;
        if (now_ns - last_sys_probe_ns >= SYS_REFRESH_NS) {
            cached_ram = get_memory_usage();
            cached_disk = get_disk_usage();
            last_sys_probe_ns = now_ns;
        }

        /* RAM */
        int ram = cached_ram;
        seg_str(&left, SB_MUT "ram " SB_RST);
        if (ram >= 0) {
            seg_bar(&left, ram, ram > 80 ? "\033[91m" : "\033[94m");
            seg_str(&left, " ");
            seg_int(&left, ram);
            seg_str(&left, "%");
        } else {
            seg_str(&left, "--");
        }

        /* Disk */
        int disk = cached_disk;
        seg_str(&left, SB_MUT " │ disk " SB_RST);
        if (disk >= 0) {
            seg_bar(&left, disk, disk > 80 ? "\033[91m" : "\033[93m");
            seg_str(&left, " ");
            seg_int(&left, disk);
            seg_str(&left, "%");
        } else {
            seg_str(&left, "--");
        }

        /* Network */
        seg_str(&left, SB_MUT " │ net " SB_RST "\033[92m▼");
        seg_bytes(&left, rx_rate);
        seg_str(&left, SB_RST " \033[94m▲");
        seg_bytes(&left, tx_rate);
        seg_str(&left, SB_RST);

        /* Battery */
        seg_battery(&left);

        /* Uptime */
        seg_str(&left, SB_MUT " │ " SB_RST);
        seg_uptime(&left, uptime_sec);
        break;
    }

    case SB_MODE_DEV: {
        /* Git branch */
        if (g_sb.cached_has_git) {
            seg_str(&left, "\033[35m⎇ ");
            seg_branch(&left, g_sb.cached_git_branch);
            seg_str(&left, " ");
            if (g_sb.cached_git_status == 1)
                seg_str(&left, "\033[33m✗");
            else if (g_sb.cached_git_status == 2)
                seg_str(&left, "\033[33m?");
            else
                seg_str(&left, "\033[32m✓");
            seg_str(&left, SB_RST);
        } else {
            seg_str(&left, SB_MUT "no repo" SB_RST);
        }

        /* Project type detection — 5-6 stat() calls, so rate-limit the
         * probe and re-check immediately when the cwd changes. */
        static char cached_proj[16];
        static char cached_proj_cwd[JASH_CWD_MAX];
        static uint64_t last_proj_probe_ns = 0;
        if (now_ns - last_proj_probe_ns >= PROJ_REFRESH_NS ||
            strcmp(cached_proj_cwd, s->cwd) != 0) {
            struct stat st;
            const char* proj;
            if (stat("CMakeLists.txt", &st) == 0 || stat("Makefile", &st) == 0)
                proj = "C/C++";
            else if (stat("package.json", &st) == 0)
                proj = "Node.js";
            else if (stat("Cargo.toml", &st) == 0)
                proj = "Rust";
            else if (stat("go.mod", &st) == 0)
                proj = "Go";
            else if (stat("requirements.txt", &st) == 0 || stat("pyproject.toml", &st) == 0)
                proj = "Python";
            else
                proj = "—";
            snprintf(cached_proj, sizeof(cached_proj), "%s", proj);
            snprintf(cached_proj_cwd, sizeof(cached_proj_cwd), "%s", s->cwd);
            last_proj_probe_ns = now_ns;
        }
        seg_str(&left, SB_MUT " │ " SB_RST);
        seg_str(&left, cached_proj[0] ? cached_proj : "—");

        /* Date + uptime */
        seg_str(&left, SB_MUT " │ " SB_RST);
        seg_str(&left, date_str);
        seg_str(&left, SB_MUT " │ " SB_RST);
        seg_uptime(&left, uptime_sec);
        break;
    }

    case SB_MODE_AI: {
        /* AI model */
        seg_str(&left, "\033[36mllama-3.3-70b" SB_RST);

        /* AI status */
        if (jash_ai_busy())
            seg_str(&left, SB_MUT " │ " SB_RST "\033[33m◐ busy" SB_RST);
        else
            seg_str(&left, SB_MUT " │ " SB_RST "\033[32m● ready" SB_RST);

        /* Auto/learn/debug flags */
        if (jash_ai_auto())  seg_str(&left, " \033[36m[auto]" SB_RST);
        if (jash_ai_learn()) seg_str(&left, " \033[33m[learn]" SB_RST);
        if (jash_ai_debug()) seg_str(&left, " \033[31m[debug]" SB_RST);

        /* Session uptime */
        seg_str(&left, SB_MUT " │ " SB_RST);
        seg_uptime(&left, uptime_sec);
        break;
    }

    case SB_MODE_COGNITIVE: {
        /* LLM mesh status (ADR-0022) */
        llm_status_t llm = llm_status_get();
        
        if (llm.connected && llm.provider != LLM_PROVIDER_NONE) {
            /* Provider icon + model */
            seg_str(&left, llm_status_get_provider_icon(llm.provider));
            seg_str(&left, " ");
            if (llm.model_name[0]) {
                seg_str(&left, llm.model_name);
            } else {
                seg_str(&left, llm_status_get_provider_name(llm.provider));
            }
            
            /* Latency indicator */
            seg_str(&left, SB_MUT " │ " SB_RST);
            if (llm.inferring) {
                seg_str(&left, "\033[33m◐" SB_RST);
            } else if (llm.last_latency_ms > 0) {
                if (llm.last_latency_ms < 50)
                    seg_str(&left, "\033[32m");  // green = fast
                else if (llm.last_latency_ms < 200)
                    seg_str(&left, "\033[33m");  // yellow = medium
                else
                    seg_str(&left, "\033[31m");  // red = slow
                seg_int(&left, llm.last_latency_ms);
                seg_str(&left, "ms" SB_RST);
            } else {
                seg_str(&left, "\033[32m●" SB_RST);
            }
            
            /* Token usage */
            if (llm.total_tokens > 0) {
                seg_str(&left, SB_MUT " │ " SB_RST);
                if (llm.total_tokens >= 1000000) {
                    seg_int(&left, (int)(llm.total_tokens / 1000000));
                    seg_str(&left, "M");
                } else if (llm.total_tokens >= 1000) {
                    seg_int(&left, (int)(llm.total_tokens / 1000));
                    seg_str(&left, "k");
                } else {
                    seg_int(&left, (int)llm.total_tokens);
                }
                seg_str(&left, " tok");
            }
        } else {
            /* No LLM connected */
            seg_str(&left, "\033[31m● offline" SB_RST);
        }
        
        /* Custom sections from modules */
        for (int i = 0; i < g_custom_section_count; i++) {
            if (g_custom_sections[i]) {
                seg_str(&left, SB_MUT " │ " SB_RST);
                g_custom_sections[i](&left, s);
            }
        }
        
        /* Session uptime */
        seg_str(&left, SB_MUT " │ " SB_RST);
        seg_uptime(&left, uptime_sec);
        break;
    }

    case SB_MODE_FOCUS: {
        /* Ultra minimal: date + battery only — the rest is silence */
        seg_str(&left, SB_MUT);
        seg_str(&left, date_str);
        seg_str(&left, SB_RST);
        seg_battery(&left);
        break;
    }

    case SB_MODE_PERSONAL: {
        seg_str(&left, date_str);

        /* Weather placeholder (future: wttr.in API) */
        seg_str(&left, SB_MUT " │ " SB_RST "\033[36m☼ 22°C" SB_RST);

        /* News placeholder (future: RSS feeds) */
        seg_str(&left, SB_MUT " │ news " SB_RST "3 new");
        break;
    }

    case SB_MODE_DEBUG: {
        /* Shell internals for debugging */
        seg_str(&left, SB_MUT "pid " SB_RST);
        seg_int(&left, getpid());
        seg_str(&left, SB_MUT " │ ppid " SB_RST);
        seg_int(&left, getppid());
        seg_str(&left, SB_MUT " │ shlvl " SB_RST);
        const char* shlvl = jash_env_get(s, "SHLVL");
        seg_str(&left, shlvl ? shlvl : "1");
        seg_str(&left, SB_MUT " │ term " SB_RST);
        const char* term = jash_env_get(s, "TERM");
        seg_str(&left, term ? term : "?");
        seg_str(&left, SB_MUT " │ cols " SB_RST);
        seg_int(&left, (int)width);
        break;
    }

    default:
        break;

    } /* end switch */

    /* ═══ COMPOSE FRAME: cursor-save · home · left · filler · right ═══ */
    uint32_t fill = 0;
    if (left.cols + right.cols < width) fill = width - left.cols - right.cols;

    char frame[JASH_STATUSBAR_MAX_WIDTH];
    uint32_t fl = 0;
    const char* head = "\0337\033[1;1H" SB_BG;
    while (*head && fl < sizeof(frame) - 1) frame[fl++] = *head++;
    for (uint32_t i = 0; i < left.len && fl < sizeof(frame) - 1; i++)
        frame[fl++] = left.buf[i];
    while (fill-- && fl < sizeof(frame) - 1) frame[fl++] = ' ';
    for (uint32_t i = 0; i < right.len && fl < sizeof(frame) - 1; i++)
        frame[fl++] = right.buf[i];
    const char* tail = "\033[0m\0338";
    while (*tail && fl < sizeof(frame) - 1) frame[fl++] = *tail++;
    frame[fl] = '\0';

    /* ═══ DOUBLE BUFFER: identical frame → zero bytes, zero flicker ═══ */
    if (!g_sb.dirty && g_sb.cached_len == fl &&
        memcmp(g_sb.cached_line, frame, fl) == 0) {
        return;
    }
    memcpy(g_sb.cached_line, frame, fl + 1);
    g_sb.cached_len = fl;

    WRITE_IGNORE(STDOUT_FILENO, frame, fl);

    g_sb.dirty = 0;
    g_sb.rendered = 1;
}

/* ─── AI Mode Indicator (when statusbar is disabled) ──────────────── */

void render_ai_mode_indicator(void) {
    if (!jash_ai_active()) return;
    if (g_sb.enabled) return;

    get_terminal_size();

    /* Paint the pill in the top-right corner and RESTORE the cursor —
     * leaving it parked on row 1 is how prompts end up drawn there. */
    char buf[96];
    int col = g_terminal_cols > 13 ? g_terminal_cols - 12 : 1;
    int n = snprintf(buf, sizeof(buf),
                     "\0337\033[1;%dH\033[K\033[1;97;48;5;124m AI MODE ON \033[0m\0338",
                     col);
    WRITE_IGNORE(STDOUT_FILENO, buf, n);
}

/* ─── Sigil Indicator (when statusbar is disabled) ──────────────── */

void render_sigil_indicator(void) {
    int lvl = jash_sigil_get();
    if (lvl == 0) return;
    if (g_sb.enabled) return;

    get_terminal_size();

    /* Level-specific label and ANSI 256-color background:
     *  LOG  (1) — blue   (33) — ◉ LOG
     *  SIGN (2) — yellow (220)— ✍ SIGN
     *  SEAL (3) — red    (124)— ⊗ SEAL  */
    static const struct { const char* label; int bg; } lvl_pill[] = {
        { "", 0 },
        { " \xe2\x97\x89 LOG  ", 33  },
        { " \xe2\x9c\x8d SIGN ", 220 },
        { " \xe2\x8a\x97 SEAL ", 124 },
    };
    int col = g_terminal_cols > 26 ? g_terminal_cols - 8 : 1;
    /* Offset left of any AI pill to avoid overlap */
    if (jash_ai_active()) col -= 14;

    char buf[96];
    int n = snprintf(buf, sizeof(buf),
                     "\0337\033[1;%dH\033[K\033[1;97;48;5;%dm%s\033[0m\0338",
                     col, lvl_pill[lvl].bg, lvl_pill[lvl].label);
    WRITE_IGNORE(STDOUT_FILENO, buf, n);
}

/* ─── Status bar clear ───────────────────────────────────────────── */

void jash_statusbar_clear(jash_writer_t w, void* ctx) {
    if (!g_sb.enabled) return;
    jash_writes(w, ctx, "\0337\033[1;1H\033[2K\0338");
    g_sb.cached_len = 0;   /* screen no longer matches the cache */
    g_sb.dirty = 1;
}
