// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// 🎵 Hear F E R M I ∞ H A R T music. No obligation. Just a wish.
//
// Module: main — boot, REPL loop, exit with v0.4 feature integration.
//
// ┌─────────────────────────────────────────────────────────────────────────┐
// │ BETA STABILIZATION LOCK ACTIVE (ADR-0035-BETA-LOCK)                   │
// │ No new features will be processed until stabilization plan is complete.  │
// │ See: BETA_STABILIZATION_PLAN.md                                         │
// └─────────────────────────────────────────────────────────────────────────┘
//
// Per manifesto §3 this file stays lean:
//   1. allocate one jash_state_t on the stack
//   2. seed env defaults + initialize v0.4 modules
//   3. paint the banner
//   4. loop: read line → highlight → eval → progress tracking → continue
//   5. on exit: flush history + cleanup modules
//
// v0.4 Integrations:
//   • Output Highlighter (INNOVATION #2): auto-detect & highlight command output
//   • Snippet Editor (INNOVATION #3): CTRL+E handler for multi-line editing
//   • Progress Bar (INNOVATION #4): auto-detect long operations (>1s)
//   • Smart Suggestions (INNOVATION #5): keystroke-driven learning from history

#include "core/state.h"
#include "core/memo.h"
#include <core/limits.h>
#include <limits.h>
#include "core/env.h"
#include "core/history.h"
#include "core/frecency.h"
#include "core/persist.h"
#include "core/cache.h"
#include "core/jash_ai.h"
#include "core/jash_brain.h"
#include "core/world.h"
#include "parser/arithmetic.h"
#include "parser/exec.h"
#include "core/jobs_bg.h"
#include "core/prefetch.h"
#include "core/drift.h"
#include "core/chain_read.h"
#include "core/token_tracker.h"
#include "core/differentiators.h"
#include "core/preexec.h"
#include "module/loader.h"
#include "module/sigil.h"
#include "journal/journal.h"
#include "theme/io.h"
#include "theme/banner.h"
#include "readline/loop.h"
#include "parser/eval.h"
#include "parser/exec.h"
#include "parser/script.h"   /* JINV-083: jash_find_keyword_set */
#include "parser/quotes.h"
#include "parser/heredoc.h"
/* Shared with the heredoc registry so escaped paths remain recognizable. */
extern size_t jash_heredoc_quote_path(const char* path, char* out, size_t cap);
extern void jash_heredoc_track_lifetime(void);
extern int jash_heredoc_may_escape(void);
#include "parser/param_expand.h"
#include "parser/cmdsub.h"
#include "parser/arithmetic.h"
#include "sys/syscalls.h"
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <locale.h>
#include "core/output_budget.h"   /* ADR-0044: budget-govern builtin output */
#ifndef JASH_TARGET_BEYOND
#include <termios.h>
#endif
#include <signal.h>
#include <stdio.h>
#include <ctype.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include "job_control.h"
#include "parser/coproc.h"
#include <builtins/builtin.h>
#include <time.h>

#include <sched.h>
#include "osif/osinfo.h"
#ifdef JASH_TARGET_MINIX
#include "osif/minix/surface.h"
static struct jash_minix_surface jash_minix_surface;
static struct jash_minix_surface *jash_surface_active;
#endif
#ifdef JASH_BETHOS
#include "osif/bethos/surface.h"
#endif
#include "boot.h"
#include "core/timeline.h"

#include "liquid/jash_liquid.h"
#include "core/guard.h"
#include "core/adopt.h"
#include "core/agent_json.h"
#include "parser/quotes.h"
#include "core/serve.h"

#if defined(__linux__) && !defined(RENAME_NOREPLACE)
#define RENAME_NOREPLACE (1U << 0)
#endif
#include "core/caps_json.h"
#include "core/agent_contract.h"
#include "core/record.h"
#include "core/context_engine.h"
#include "core/stance.h"
#include "output/highlighter.h"
#include "editor/snippet.h"
#include "ui/progress.h"
#include "ui/statusbar.h"
#include "completion/smart_suggestions.h"
#ifdef JASH_TINALINUX
#include "osif/tina/surface.h"
extern volatile pid_t g_jash_fg_pgid;
static void jash_sigint_forward(int sig) {
    pid_t pgid = g_jash_fg_pgid;
    if (pgid > 0) kill(-pgid, sig);
}
#endif

/* ── Wave 10: Beyond Surface native API ────────────────────────────────────
 * When running on a Beyond surface, jash can detect it via ioctl(SURF_GET_INFO)
 * and emit typed cells (ERROR, PROGRESS, TABLE) via ioctl(SURF_EMIT_CELL).
 * This is the integration point: jash speaks the surface's native cell protocol
 * instead of relying on ANSI escape sequences. Backward compatible: if the
 * ioctl fails, jash falls back to regular write(). F E R M I ∞ H A R T */
#ifdef JASH_TARGET_BEYOND

/* Mirror of kernel's surface_info_t (syscall.h). */
#define SURF_GET_INFO_IOCTL  0x5301
#define SURF_EMIT_CELL_IOCTL 0x5302
#define SURF_MAGIC_VAL       0x53555246  /* 'SURF' */

/* BLOOM cell types (mirror kern/surface.c). */
#define BLOOM_CELL_BYTE     1
#define BLOOM_CELL_KV       2
#define BLOOM_CELL_TABLE    3
#define BLOOM_CELL_DIAG     4
#define BLOOM_CELL_PROGRESS 5
#define BLOOM_CELL_PROMPT   6
#define BLOOM_CELL_ERROR    7
#define BLOOM_CELL_INPUT    8

typedef struct {
    unsigned int magic;
    unsigned int version;
    int          surf_id;
    unsigned int capabilities;
    unsigned short width, height;
    unsigned int cell_types;
} jash_surface_info_t;

typedef struct {
    unsigned int cell_type;
    unsigned int len;
    char         data[256];
} jash_surf_cell_t;

/* Global: is jash running on a Beyond surface? Set at startup. */
static int g_on_beyond_surface = 0;
static int g_beyond_surf_id = 0;

/* ioctl wrapper — blibc exposes bear_ioctl with variadic args.
 * On Beyond, this routes through syscall 0x67. */
extern int bear_ioctl(int fd, unsigned long request, ...);

static __attribute__((unused)) int jash_surface_detect(void) {
    jash_surface_info_t info;
    memset(&info, 0, sizeof(info));
    int rc = bear_ioctl(1, (unsigned long)SURF_GET_INFO_IOCTL, &info);
    if (rc == 0 && info.magic == SURF_MAGIC_VAL) {
        g_on_beyond_surface = 1;
        g_beyond_surf_id = info.surf_id;
        return 1;
    }
    return 0;
}

/* Emit a typed cell to the Beyond surface via ioctl. */
static int jash_surface_emit_cell(unsigned int cell_type, const char *data, unsigned int len) {
    if (!g_on_beyond_surface) return -1;
    jash_surf_cell_t cell;
    memset(&cell, 0, sizeof(cell));
    cell.cell_type = cell_type;
    if (len > 255) len = 255;
    cell.len = len;
    memcpy(cell.data, data, len);
    cell.data[len] = '\0';
    return bear_ioctl(1, (unsigned long)SURF_EMIT_CELL_IOCTL, &cell);
}

/* Convenience: emit an error cell. */
static __attribute__((unused)) void jash_surface_emit_error(const char *msg) {
    unsigned int len = 0;
    while (msg[len]) len++;
    jash_surface_emit_cell(BLOOM_CELL_ERROR, msg, len);
}

/* Convenience: emit a progress cell. Format: "current/total label". */
static __attribute__((unused)) void jash_surface_emit_progress(unsigned int current, unsigned int total, const char *label) {
    char buf[128];
    int pos = 0;
    /* Simple integer to string without sprintf (freestanding). */
    buf[pos++] = '[';
    if (total > 0) {
        unsigned int pct = (current * 100) / total;
        if (pct >= 100) { buf[pos++] = '1'; buf[pos++] = '0'; buf[pos++] = '0'; }
        else if (pct >= 10) { buf[pos++] = (char)('0' + pct / 10); buf[pos++] = (char)('0' + pct % 10); }
        else { buf[pos++] = (char)('0' + pct); }
    }
    buf[pos++] = '%';
    buf[pos++] = ']';
    buf[pos++] = ' ';
    while (*label && pos < 120) buf[pos++] = *label++;
    buf[pos] = '\0';
    jash_surface_emit_cell(BLOOM_CELL_PROGRESS, buf, (unsigned int)pos);
}

/* Convenience: emit a table cell. Format: "header|row1|row2|...". */
static __attribute__((unused)) void jash_surface_emit_table(const char *header, const char **rows, int nrows) {
    char buf[256];
    int pos = 0;
    while (*header && pos < 250) buf[pos++] = *header++;
    for (int i = 0; i < nrows && pos < 250; i++) {
        buf[pos++] = '|';
        const char *r = rows[i];
        while (*r && pos < 250) buf[pos++] = *r++;
    }
    buf[pos] = '\0';
    jash_surface_emit_cell(BLOOM_CELL_TABLE, buf, (unsigned int)pos);
}

#else
/* Non-Beyond: stubs that are no-ops. */
/* static int jash_surface_detect(void) { return 0; } */
/* static void jash_surface_emit_error(const char *msg) { (void)msg; }
static void jash_surface_emit_progress(unsigned int c, unsigned int t, const char *l) {
    (void)c; (void)t; (void)l;
}
static void jash_surface_emit_table(const char *h, const char **r, int n) {
    (void)h; (void)r; (void)n;
} */
#endif /* JASH_TARGET_BEYOND */

static double _bench_now(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/* v0.4 Feature contexts — real implementations */
static jash_highlighter_t* g_highlighter = NULL;
static jash_editor_session_t* g_editor_session = NULL;
static jash_progress_t* g_progress_bar = NULL;
static jash_ss_engine_t* g_suggestions_engine = NULL;

static int repl_is_plain_exit(const char* p) {
    if (!p) return 0;
    while (*p == ' ' || *p == '\t') p++;
    if (p[0] != 'e' || p[1] != 'x' || p[2] != 'i' || p[3] != 't') return 0;
    p += 4;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '\0') return 1;
    if (*p == '+' || *p == '-') p++;
    if (!isdigit((unsigned char)*p)) return 0;
    while (isdigit((unsigned char)*p)) p++;
    while (*p == ' ' || *p == '\t') p++;
    return *p == '\0';
}

static int repl_rename_noreplace(int dirfd, const char* from, const char* to) {
#if defined(__linux__)
    return jash_osif_renameat2(dirfd, from, dirfd, to, RENAME_NOREPLACE);
#elif defined(__APPLE__)
    return renameatx_np(dirfd, from, dirfd, to, RENAME_EXCL);
#else
    (void)dirfd; (void)from; (void)to;
    errno = ENOTSUP;
    return -1;
#endif
}

static int repl_open_parent(const char* path, char* name, size_t name_cap) {
    char parent[512];
    if (!path || strlen(path) >= sizeof(parent)) { errno = ENAMETOOLONG; return -1; }
    strlcpy(parent, path, sizeof(parent));
    char* slash = strrchr(parent, '/');
    const char* base = path;
    if (!slash) {
        strlcpy(parent, ".", sizeof(parent));
    } else {
        base = strrchr(path, '/') + 1;
        if (slash == parent) slash[1] = '\0';
        else *slash = '\0';
    }
    if (!*base || strlen(base) >= name_cap) { errno = ENAMETOOLONG; return -1; }
    strlcpy(name, base, name_cap);
    return open(parent, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
}

/* Probe the exact no-replace primitive without changing the namespace. A
 * supported implementation reports EEXIST when source and destination are the
 * same live name. Platforms without the primitive fail before the heredoc's
 * consumer can run, rather than discovering that cleanup is unsafe afterward. */
static int repl_preflight_heredoc(const char* path, int bodyfd) {
    struct stat opened, named;
    char name[256];
    if (bodyfd < 0 || fstat(bodyfd, &opened) != 0) return -1;
    int parent_fd = repl_open_parent(path, name, sizeof(name));
    if (parent_fd < 0) return -1;
    int identity_ok = fstatat(parent_fd, name, &named, AT_SYMLINK_NOFOLLOW) == 0 &&
                      S_ISREG(named.st_mode) && named.st_dev == opened.st_dev &&
                      named.st_ino == opened.st_ino;
    int supported = 0;
    if (identity_ok) {
        errno = 0;
        int rc = repl_rename_noreplace(parent_fd, name, name);
        supported = rc == 0 || (rc < 0 && errno == EEXIST);
    }
    close(parent_fd);
    if (!supported) errno = ENOTSUP;
    return supported ? 0 : -1;
}

/* Capture the pathname into a private no-replace name before deleting it. This
 * makes a raced replacement observable instead of unlinking an unrelated file. */
static int repl_cleanup_heredoc(const char* path, int bodyfd) {
    struct stat opened, captured;
    char name[256], hold[96] = "";
    static unsigned long serial;
    int parent_fd = bodyfd >= 0 && fstat(bodyfd, &opened) == 0
        ? repl_open_parent(path, name, sizeof(name)) : -1;
    int removed = 0;

    if (parent_fd >= 0) {
        for (int attempt = 0; attempt < 64; attempt++) {
            snprintf(hold, sizeof(hold), ".jash-heredoc-clean-%ld-%lu",
                     (long)getpid(), ++serial);
            if (repl_rename_noreplace(parent_fd, name, hold) == 0) break;
            if (errno != EEXIST) { hold[0] = '\0'; break; }
        }
        if (hold[0] &&
            fstatat(parent_fd, hold, &captured, AT_SYMLINK_NOFOLLOW) == 0 &&
            captured.st_dev == opened.st_dev && captured.st_ino == opened.st_ino &&
            captured.st_nlink == 1) {
            removed = unlinkat(parent_fd, hold, 0) == 0 && fsync(parent_fd) == 0;
        } else if (hold[0]) {
            /* The captured name was a replacement. Put it back only if doing
             * so cannot overwrite a newer entry; otherwise retain it in hold. */
            (void)repl_rename_noreplace(parent_fd, hold, name);
        }
        close(parent_fd);
    }
    if (!removed) {
        int scrubbed = bodyfd >= 0 && ftruncate(bodyfd, 0) == 0 &&
                       fsync(bodyfd) == 0;
        if (scrubbed)
            fprintf(stderr,
                    "jash: heredoc: cleanup failed; body scrubbed; status 2\n");
        else
            fprintf(stderr,
                    "jash: heredoc: cleanup failed; body may remain; status 2\n");
    }
    int close_ok = bodyfd < 0 || close(bodyfd) == 0;
    if (removed && !close_ok)
        fprintf(stderr, "jash: heredoc: cleanup descriptor close failed; status 2\n");
    return removed && close_ok ? 0 : -1;
}

/* If stdin is a tty but is currently in raw/non-canonical mode (e.g. after
 * `exec jash` from a jash whose readline left the terminal raw), force it
 * back to a sane canonical state before we save it as "original". Otherwise
 * enable_raw_mode() saves the broken raw state and restore leaves the user
 * with a dead/echo-less terminal. */
#ifndef JASH_TARGET_BEYOND
#ifdef JASH_TINALINUX
static int input_is_interactive(void) {
    return jash_tina_surface_native_input() || isatty(STDIN_FILENO);
}
static int output_is_interactive(void) {
    return jash_tina_surface_native_output() || isatty(STDOUT_FILENO);
}
#endif
static void terminal_ensure_sane(void) {
#ifdef JASH_TINALINUX
    if (jash_tina_surface_native_input()) {
        (void)jash_tina_surface_editor_begin();
        return;
    }
#endif
    if (!isatty(STDIN_FILENO)) return;
    struct termios t;
    if (tcgetattr(STDIN_FILENO, &t) != 0) return;
    if (!(t.c_lflag & ICANON) || !(t.c_lflag & ECHO)) {
        t.c_lflag |= ICANON | ECHO | ISIG | IEXTEN;
        t.c_iflag |= BRKINT | ICRNL | INPCK | ISTRIP | IXON;
        t.c_oflag |= OPOST;
        t.c_cc[VMIN] = 1;
        t.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &t);
    }
}

static int repl_confirm_exit(void) {
    const char* mode = getenv("JASH_EXIT_CONFIRM");
    if (mode && (!strcmp(mode, "0") || !strcmp(mode, "off") || !strcmp(mode, "never")))
        return 1;
#ifdef JASH_TINALINUX
    if (!input_is_interactive() || !output_is_interactive()) return 1;
    int native_input = jash_tina_surface_native_input();
    if (native_input && jash_tina_surface_editor_begin() != 0) return 0;
#else
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) return 1;
#endif

    struct termios oldt, raw;
#ifdef JASH_TINALINUX
    int raw_ok = !native_input && (tcgetattr(STDIN_FILENO, &oldt) == 0);
#else
    int raw_ok = (tcgetattr(STDIN_FILENO, &oldt) == 0);
#endif
    if (raw_ok) {
        raw = oldt;
        raw.c_lflag &= (tcflag_t)~(ICANON | ECHO);
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    }

    static const char msg[] =
        "\033[1;33mexit?\033[0m Enter fecha o shell · Esc/Ctrl+C volta\r\n";
    WRITE_IGNORE(STDOUT_FILENO, msg, sizeof(msg) - 1);

    int confirm = 0;
    for (;;) {
        unsigned char c = 0;
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        if (c == '\r' || c == '\n') { confirm = 1; break; }
        if (c == 0x1b || c == 0x03 || c == 'q' || c == 'Q') break;
    }

    if (raw_ok) tcsetattr(STDIN_FILENO, TCSAFLUSH, &oldt);
#ifdef JASH_TINALINUX
    if (native_input && jash_tina_surface_editor_end() != 0) return 0;
#endif
    if (!confirm) {
        static const char cancel[] = "\033[90mexit cancelado\033[0m\r\n";
        WRITE_IGNORE(STDOUT_FILENO, cancel, sizeof(cancel) - 1);
    }
    return confirm;
}
#else
/* Beyond: no termios — stubs that are no-ops / always decline. */
static void terminal_ensure_sane(void) { /* no termios on Beyond */ }
static int repl_confirm_exit(void) { return 1; /* always confirm */ }
#endif /* JASH_TARGET_BEYOND */

/* On Josh, all sys_* primitives have real bodies in
 * sys/josh_syscalls.c (linked in only for the Josh target). main.c
 * stays thin — just the REPL. The exit builtin signals shutdown via
 * state.interactive=0; sys_exit_or_loop_break exists as a hook for
  * platform-specific main loops (e.g. macOS RunLoop integration). */
 __attribute__((used)) static void sys_exit_or_loop_break(int code) { (void)code; }

/* ════════════════════════════════════════════════════════════════════════════
   MODULE CONTEXT: Global state for v0.4 features
   ════════════════════════════════════════════════════════════════════════════ */

/* v0.4 stubs removed — see git history if needed */

/* ════════════════════════════════════════════════════════════════════════════
   I/O ENDPOINTS
   ════════════════════════════════════════════════════════════════════════════ */

/* The two I/O endpoints jash needs: stdin reader + stdout writer.
 * Both wrap sys_read / sys_write through the standard fd numbers. */

static int jash_stdin_reader(void* ctx, char* out) {
    (void)ctx;
#ifdef JASH_TARGET_BEYOND
    /* INV-PERF10-JASH-PASTE-001: Beyond's raw surface read is bounded at 256
     * bytes. Drain each kernel transfer from process memory instead of paying
     * one syscall per decoder byte; the reader API still returns one byte. */
    static char rbuf[256];
    static size_t rbuf_len = 0, rbuf_pos = 0;
    if (rbuf_pos < rbuf_len) {
        *out = rbuf[rbuf_pos++];
        return 1;
    }
    rbuf_len = rbuf_pos = 0;
    long n = read(0, rbuf, sizeof(rbuf));
    if (n == 0) return 0;   /* EOF */
    if (n < 0) {
        if (errno == EINTR) {
#ifdef JASH_SURFACE_TERMIOS
            return 3; /* INV-CONSOLE-004: CTRL event, not idle timeout. */
#else
            return 2;
#endif
        }
        return -1;  /* Error */
    }
    rbuf_len = (size_t)n;
    rbuf_pos = 1;
    *out = rbuf[0];
    return 1;  /* Got data */
#else
    /* Buffered read: drain a 4KB buffer before the next select(). A
     * multi-KB paste previously cost one select+read syscall pair per
     * byte (~20k syscalls for 10KB); now it costs ~3 read() calls.
     * Delivery semantics are unchanged: exactly one byte per call. */
    static char rbuf[4096];
    static size_t rbuf_len = 0, rbuf_pos = 0;

    if (rbuf_pos < rbuf_len) {
        *out = rbuf[rbuf_pos++];
        return 1;  /* Buffered byte */
    }
    rbuf_len = rbuf_pos = 0;

    /* Use select() with timeout for non-blocking read */
    fd_set readfds;
    struct timeval tv;

    FD_ZERO(&readfds);
    FD_SET(0, &readfds);
    tv.tv_sec = 0;
    tv.tv_usec = 100000;  /* 100ms timeout */

    int rc = select(1, &readfds, NULL, NULL, &tv);

    if (rc < 0) {
        if (errno == EINTR) return 2;  /* Interrupted, try again */
        return -1;  /* Error */
    }
    if (rc == 0) return 2;  /* Timeout, no data */

    /* Data available, read it */
    long n = read(0, rbuf, sizeof(rbuf));
    if (n == 0) return 0;  /* EOF */
    if (n < 0) return -1;  /* Error */
    rbuf_len = (size_t)n;
    rbuf_pos = 1;
    *out = rbuf[0];
    return 1;  /* Got data */
#endif
}

static long jash_stdout_writer(void* ctx, const char* buf, size_t n) {
    (void)ctx;
#ifdef JASH_TARGET_MINIX
    if (jash_surface_active)
        return jash_minix_surface_writer(jash_surface_active, buf, n);
#endif
    return sys_write(1, buf, (unsigned long)n);
}

/* ════════════════════════════════════════════════════════════════════════════
   v0.4 FEATURE: HIGHLIGHTED OUTPUT DISPLAY (STUB)
   Placeholder for output highlighter integration
   ════════════════════════════════════════════════════════════════════════════ */

/* Output writer with highlighting */
static long jash_highlighted_stdout_writer(void* ctx, const char* buf, size_t n) {
    (void)ctx;
#ifdef JASH_TARGET_MINIX
    if (jash_surface_active)
        return jash_minix_surface_writer(jash_surface_active, buf, n);
#endif
    /* For now, disable highlighting to avoid buffering issues.
     * TODO: Fix highlighter buffering and re-enable. */
    return sys_write(1, buf, (unsigned long)n);
}

/* History path — uses HOME from environment */
static char jash_hist_path[512] = {0};

static void init_hist_path(const jash_state_t* s) {
    const char *home = jash_env_get(s, "HOME");
    if (!home) home = jash_tmpdir();
    unsigned i = 0;
    while (*home && i < sizeof(jash_hist_path) - 20) { jash_hist_path[i++] = *home++; }
    if (i > 0 && jash_hist_path[i-1] != '/') jash_hist_path[i++] = '/';
    const char *name = ".jash_history";
    while (*name && i < sizeof(jash_hist_path) - 1) { jash_hist_path[i++] = *name++; }
    jash_hist_path[i] = '\0';
}

static void ensure_repl_features(jash_state_t* state) {
    static int loaded = 0;
    if (loaded) return;
    loaded = 1;

    frecency_init(&state->frecency, jash_env_get(state, "HOME"));
    frecency_load(&state->frecency);
    predict_init(&state->predictions, jash_env_get(state, "HOME"));
    predict_load(&state->predictions);

    g_highlighter = jash_highlighter_create();
    g_progress_bar = jash_progress_create();
    g_suggestions_engine = jash_ss_engine_create(state);
    state->ss_engine = g_suggestions_engine;
    jash_ss_engine_load(g_suggestions_engine);
}

static int is_simple_command_line(const char* s) {
    if (!s) return 0;
    while (*s) {
        switch (*s) {
        case ';': case '|': case '&': case '<': case '>':
        case '=': case '$': case '`': case '*': case '?': case '[': case ']':
        case '{': case '}': case '(': case ')': case '\'': case '"':
        case '~': case '\\':
            return 0;
        default:
            break;
        }
        s++;
    }
    return 1;
}

/* JINV-019: quote-aware (parser/quotes.c) — era a terceira cópia do mesmo
 * corte-no-espaço. Hoje inalcançável com aspas (is_simple_command_line
 * aborta antes), mas a divergência é a dívida: uma lógica, um lugar. */
static const char* split_simple_command(const char* line, char* name, unsigned cap) {
    return jash_first_word(line, name, cap, 0, (int*)0);
}

static int eval_command_mode_fast(jash_state_t* state, jash_writer_t w, void* ctx,
                                  const char* line, int* handled) {
    *handled = 0;
    /* Skip blank lines and comments (bash: a `#`-only line returns 0
     * silently). The slow path also checks this, but the fast path
     * runs FIRST and would otherwise try to exec `#` as a command. */
    const char* sp = line;
    while (*sp == ' ' || *sp == '\t') sp++;
    if (!*sp || *sp == '#') { *handled = 1; return 0; }

    /* Trailing `# comment` strip (bash parity). The slow path strips this
     * in jash_eval_line_inner, but the fast path bypasses that — so a
     * simple `echo a #note` would otherwise hand "a #note" to echo. */
    static char trimmed[8192];
    {
        char sq = 0, dq = 0;
        int prev_boundary = 1;
        unsigned cut = 0;
        int found = 0;
        for (unsigned i = 0; line[i]; i++) {
            char c = line[i];
            if (sq) { if (c == '\'') sq = 0; prev_boundary = 0; continue; }
            if (dq) {
                if (c == '\\' && line[i + 1]) { i++; prev_boundary = 0; continue; }
                if (c == '"') dq = 0;
                prev_boundary = 0; continue;
            }
            if (c == '\\' && line[i + 1]) { i++; prev_boundary = 0; continue; }
            if (c == '\'') { sq = 1; prev_boundary = 0; continue; }
            if (c == '"')  { dq = 1; prev_boundary = 0; continue; }
            if (c == '#' && prev_boundary) { cut = i; found = 1; break; }
            prev_boundary = (c == ' ' || c == '\t' || c == ';' ||
                             c == '&' || c == '|' || c == '(' || c == ')');
        }
        if (found) {
            if (cut >= sizeof(trimmed)) cut = sizeof(trimmed) - 1;
            for (unsigned i = 0; i < cut; i++) trimmed[i] = line[i];
            trimmed[cut] = '\0';
            /* JINV-063: era um rtrim CEGO aqui — a quarta cópia da mesma ideia,
             * e a única errada: `echo x\  #nota` perdia também o espaço
             * ESCAPADO, que é DADO. Uma lógica, um lugar (parser/quotes.h). */
            cut = (unsigned)jash_rtrim_unquoted(trimmed);
            trimmed[cut] = '\0';
            if (!trimmed[0]) { *handled = 1; return 0; }
            line = trimmed;
        }
    }

    /* JINV-063 — a SEGUNDA porta. Esta via rápida de `-c` não passa por
     * jash_eval_line, então o corte de espaço final feito lá (parser/eval.c,
     * no ponto de split) nunca a alcançava: `jash -c 'echo x '` continuava
     * imprimindo `x `. Mesma função auxiliar, nenhuma lógica duplicada —
     * duplicar a varredura de aspas foi o pior achado desta auditoria.
     * Contrato e motivo em parser/quotes.h. */
    {
        size_t keep = jash_rtrim_unquoted(line);
        if (line[keep] != '\0') {
            static char* jinv063_slot;   /* alocado só se alguém precisar */
            if (!jinv063_slot) jinv063_slot = (char*)malloc(JASH_LINE_CAP);
            if (jinv063_slot && keep < (size_t)JASH_LINE_CAP) {
                memcpy(jinv063_slot, line, keep);
                jinv063_slot[keep] = '\0';
                line = jinv063_slot;     /* nunca trunca: não coube, segue intacta */
            }
        }
    }

    if (!is_simple_command_line(line)) return 0;

    char name[512];
    const char* args = split_simple_command(line, name, sizeof(name));
    if (!name[0]) { *handled = 1; return 0; }

    if (strcmp(name, "cd..") == 0 && (!args || !*args)) {
        *handled = 1;
        return jash_b_cd(state, w, ctx, "..");
    }

    /* Keywords must go through full eval (they have special syntax).
     * Without this, 'coproc', 'if', 'for', etc. would be treated as
     * external commands and fail with 'command not found'. */
    if (strcmp(name, "coproc") == 0 || strcmp(name, "if") == 0 ||
        strcmp(name, "for") == 0 || strcmp(name, "while") == 0 ||
        strcmp(name, "until") == 0 || strcmp(name, "case") == 0 ||
        strcmp(name, "select") == 0 || strcmp(name, "function") == 0 ||
        strcmp(name, "time") == 0 ||
        /* JINV-066: `!` e PALAVRA RESERVADA (negacao de pipeline), nao nome
         * de programa. Esta via rapida de `-c` extraia `!` como comando e
         * saia procurando um executavel chamado `!` — rc 127 para todo
         * `! cmd` de linha unica, inclusive dentro de `until`/`if`. Mesma
         * classe do JINV-063: a segunda porta que nao conhecia a regra da
         * primeira. A negacao mora em parser/eval.c. */
        strcmp(name, "!") == 0) {
        *handled = 0;  /* fall through to jash_eval_line */
        return 0;
    }

    /* ADR-0040 W3: absorbable `bash -c` must reach jash_exec_external, where
     * the pre-exec engine rewrites it into a jash re-exec — skip the bypass
     * guard so strict mode upgrades instead of blocking. */
    if (!jash_preexec_will_absorb(name, args) &&
        jash_guard_bypass_detect(name, args, w, ctx)) {
        *handled = 1;
        return 126;  /* command not executable by guard policy */
    }

    const jash_builtin_t* b = jash_builtin_lookup(name);
    if (b) {
        *handled = 1;
        /* ADR-0044: budget-govern a simple builtin's output here too — this
         * fast `-c` path (recording off) bypasses eval.c's governed dispatch.
         * Same gate: non-tty stdout + JASH_LLM_BUDGET set → cap + spill so
         * `cat bigfile` never floods the window. No memo on this path.
         * jash_exec_stdout_is_session(): capar SÓ o que vai para a janela da
         * sessão. Se o fd 1 já é um pipe ou um `> arquivo`, capar não protege
         * contexto nenhum — só perde dado. */
        if (jash_budget_limit() > 0 && !isatty(STDOUT_FILENO) &&
            jash_exec_stdout_is_session()) {
            jash_budget_wrap_t bw;
            jash_budget_wrap_init(&bw, w, ctx, name);
            int rc = jash_builtin_dispatch(b, state, jash_budget_wrap_writer(&bw), &bw, args);
            jash_budget_wrap_finish(&bw, w, ctx);
            return rc;
        }
        return jash_builtin_dispatch(b, state, w, ctx, args);
    }

    *handled = 1;
    return jash_exec_external(state, w, ctx, name, args);
}

/* Heredoc / here-string bodies are materialized into temp files during the
 * preprocessing pass below and registered (raw) with the heredoc module. Their
 * $VAR/$(..)/$((..)) expansion is RE-ENTRANT: it runs against the current shell
 * state every time the owning line executes, driven from the top of
 * jash_eval_line — so a heredoc inside a for/while loop sees the per-iteration
 * value of the loop variable (BUG-030). See c/parser/heredoc.c:
 * jash_heredoc_register / jash_heredoc_expand_line. */

/* BUG-036: is `pos` outside any '...'/"..." quote, given the buffer starts
 * unquoted at `start`? The old heredoc detection was a bare
 * strstr(rp, "<<") with no quote-state tracking, so any command argument
 * that merely CONTAINS the literal text "<<" (e.g. `cmd <<'EOF'` embedded
 * inside a larger quoted string, or `-c "printf 'a <<'\''b'\'' c'"`) got
 * rewritten as a real heredoc operator, corrupting the command entirely —
 * not just the cosmetic tmpfile-path leak the original report described.
 * Backslash only escapes inside double quotes (POSIX: single quotes take
 * everything literally, including backslash). */
static int jash_pos_is_unquoted(const char* start, const char* pos) {
    char q = 0;
    int depth = 0;   /* BUG-113: inside $(…) / $((…)) / ${…} */
    for (const char* p = start; p < pos && *p; p++) {
        char c = *p;
        if (q) {
            if (c == '\\' && q == '"' && p[1]) { p++; continue; }
            if (c == q) q = 0;
            continue;
        }
        if (c == '\'' || c == '"') { q = c; continue; }
        if (c == '\\' && p[1]) { p++; continue; }
        /* BUG-113: a `<<` inside an ARITHMETIC/brace expansion — $((2<<3)),
         * ${…} — is not a heredoc operator. Track that depth like
         * line_has_op. A word-start `((` opens an arithmetic STATEMENT
         * (bash reserves `((` for arithmetic), so ((z<<=3)) is covered too.
         * BUG-090: plain `$(` is a COMMAND context — a heredoc inside
         * $(cat <<EOF …) IS a heredoc and must be preprocessed, so single
         * paren is transparent at top level. */
        if (c == '$' && p[1] == '(') {
            if (p[2] == '(') { depth += 2; p += 2; }   /* $(( arithmetic */
            else if (depth > 0) { depth++; p++; }      /* nested: keep balance */
            /* else: top-level $( cmdsub — transparent */
            else { p++; }
            continue;
        }
        if (c == '$' && p[1] == '{') { depth++; p++; continue; }
        if (depth == 0 && c == '(' && p[1] == '(' &&
            (p == start || p[-1] == ' ' || p[-1] == '\t' || p[-1] == ';' ||
             p[-1] == '&' || p[-1] == '|' || p[-1] == '\n')) {
            depth += 2; p++; continue;
        }
        if (depth > 0) {
            if (c == '(' || c == '{') depth++;
            else if (c == ')' || c == '}') depth--;
        }
    }
    return q == 0 && depth == 0;
}

/* strstr(rp, "<<") that skips any match sitting inside an open quote,
 * re-scanning from `buf_start` (the true beginning of the buffer) each time
 * a candidate needs a parity check — see jash_pos_is_unquoted. */
static const char* jash_find_unquoted_heredoc_op(const char* buf_start, const char* rp) {
    const char* hd = strstr(rp, "<<");
    while (hd && !jash_pos_is_unquoted(buf_start, hd)) {
        hd = strstr(hd + 2, "<<");
    }
    return hd;
}

/* Execute a multi-line program (script body or a `-c` string containing
 * newlines). Performs heredoc preprocessing, POSIX backslash-newline
 * continuation, and compound-statement (if/for/while/case/select/function)
 * accumulation, then evaluates one logical statement at a time. Shared by
 * script mode and the `-c` path so both honor multi-line semantics
 * identically. Returns the last statement's exit code. F E R M I ∞ H A R T */
#define JASH_SCRIPT_PROC_CAP JASH_LINE_CAP  /* JINV-030: deriva de core/limits.h — nao um numero solto */

/* ═══ JINV-081 — o acumulador de enunciado composto não pode ter teto mudo ══
 *
 * `jash_run_multiline` monta UM enunciado lógico por vez num buffer e só
 * então chama o avaliador. Esse buffer era `char sline[4096]`, e todo laço
 * que o enchia parava em `si < sizeof(sline) - 1` — em silêncio. No
 * acumulador de composto o corte era explícito:
 *
 *     if (si >= sizeof(sline) - 1) { sline[sizeof(sline) - 1] = '\0'; break; }
 *
 * O enunciado saía sem a cauda, e o avaliador recebia um `for` sem o `done`,
 * um `if` sem o `fi`. Penhasco medido (arquivo inteiro, corpo de recheio):
 *
 *     for    4043 B  OK   │ for    4713 B  "missing done"
 *     if     4040 B  OK   │ if     4710 B  "missing fi"
 *     while  4061 B  OK   │ while  4731 B  "missing done"
 *
 * Todo `configure` de autoconf abre com um bloco
 * `for ac_option / do … case … esac / done` de ~7 KB. Nenhum dos 19 medidos
 * conseguia atravessá-lo: 28 das 38 execuções morriam exatamente aí.
 *
 * O mesmo defeito, no mesmo laço, em mais dois pontos:
 *   - `char ppeek[4096]`: a linha FÍSICA lida por vez também truncava, então
 *     uma linha longa perdia a cauda dentro de um composto;
 *   - `pbuf` em JASH_SCRIPT_PROC_CAP: o script pré-processado parava em
 *     128 KiB sem dizer nada — 11 dos 19 `configure` são maiores que isso.
 *
 * Conserto (idioma do JINV-007/022/080): buffer que CRESCE. Os 4096 bytes de
 * pilha continuam existindo como caminho rápido — agora são um limiar de
 * DESEMPENHO, não um teto de dado — e ficam muito abaixo de
 * JASH_LINE_CAP_STACK (32 KiB, core/limits.h). Acima disso o buffer migra
 * para o heap e dobra conforme precisa. Falta de memória é recusa ALTA
 * (stderr + rc != 0, o enunciado NÃO roda), nunca truncamento.
 *
 * Gate: tests/test_jinv081_compound_accumulator.sh */
typedef struct {
    char*  buf;        /* onde escrever: aponta para `inl` ou para `heap` */
    char*  heap;       /* != NULL depois da primeira migração pilha→heap */
    size_t cap;        /* capacidade de `buf` em bytes, NUL incluso */
    char   inl[4096];  /* caminho rápido de pilha (o antigo teto, agora só um
                        * limiar de desempenho) */
} jash_accum_t;

static void jash_accum_init(jash_accum_t* a) {
    a->buf = a->inl; a->heap = NULL; a->cap = sizeof(a->inl);
}

static void jash_accum_free(jash_accum_t* a) {
    if (a->heap) free(a->heap);
    a->heap = NULL; a->buf = a->inl; a->cap = sizeof(a->inl);
}

/* Garante `need` bytes em `a->buf` (NUL incluso). `used` = bytes já escritos,
 * copiados na primeira migração pilha→heap. Devolve 0 SÓ quando falta memória
 * — e aí quem chamou recusa ALTO, nunca trunca. JINV-081 */
/* Versão para um buffer nu (o `pbuf` do pré-processamento): dobra até caber,
 * preserva o conteúdo, e devolve 0 SÓ sem memória — o antigo fica intacto.
 * JINV-081 */
static int jash_buf_grow(char** buf, size_t* cap, size_t need) {
    if (need <= *cap) return 1;
    size_t ncap = *cap ? *cap : 4096;
    while (ncap < need) {
        size_t doubled = ncap * 2;
        if (doubled <= ncap) return 0;        /* estouro de size_t */
        ncap = doubled;
    }
    char* n = (char*)realloc(*buf, ncap);
    if (!n) return 0;
    *buf = n; *cap = ncap;
    return 1;
}

static int jash_accum_reserve(jash_accum_t* a, size_t used, size_t need) {
    if (need <= a->cap) return 1;
    size_t ncap = a->cap;
    while (ncap < need) {
        size_t doubled = ncap * 2;
        if (doubled <= ncap) return 0;        /* estouro de size_t */
        ncap = doubled;
    }
    char* n = (char*)realloc(a->heap, ncap);
    if (!n) return 0;
    if (!a->heap && used) memcpy(n, a->inl, used);
    a->heap = n; a->buf = n; a->cap = ncap;
    return 1;
}

/* JINV-095 — um pedido de heredoc coletado na FASE 1 (varredura da linha) e
 * consumido na FASE 2 (captura dos corpos). O nome do temporário tem que ser
 * conhecido ANTES do corpo para que ` < <nome>` saia na posição certa da
 * linha; por isso o `mkstemp` acontece na fase 1 e o fd fica aberto até a
 * fase 2 escrever. `JASH_HD_PER_LINE` é um limite de SLOTS (quantos
 * operadores `<<` cabem numa linha lógica), não um teto de DADO: o que passar
 * dele sai como texto literal, exatamente como saía antes do segundo em
 * diante — nunca truncado em silêncio (JINV-029). O bash usa 10; 16 dá folga
 * e cobre tudo que os 19 `configure` do corpus contêm (máximo medido: 2). */
#ifndef JASH_HD_PER_LINE
#define JASH_HD_PER_LINE 16
#endif
typedef struct {
    char name[256];    /* caminho do temporário (mkstemp já resolvido) */
    char delim[256];   /* palavra delimitadora, JÁ SEM CITAÇÃO (JINV-092) */
    int  indented;     /* <<- : tira os TABs do corpo e do fechamento */
    int  no_expand;    /* delimitador citado ⇒ corpo literal (JINV-092) */
    int  fd;           /* fd do mkstemp, aberto entre as duas fases */
} hd_req_t;

/* Materialized files outlive a multiline/source call: stored functions and
 * EXIT handlers can still reference them. Expansion text has a separate scope. */
typedef struct hd_temp {
    struct hd_temp* next;
    pid_t owner;
    dev_t dev;
    ino_t ino;
    char path[];
} hd_temp_t;
static hd_temp_t* hd_temps;

static int hd_mkstemp(char* name) {
    int fd = mkstemp(name);
    if (fd < 0) return -1;
    char path[PATH_MAX];
    struct stat st;
    hd_temp_t* temp = NULL;
    if (realpath(name, path) && fstat(fd, &st) == 0)
        temp = malloc(sizeof *temp + strlen(path) + 1);
    if (!temp) {
        int saved_errno = errno;
        close(fd);
        unlink(name);
        errno = saved_errno;
        return -1;
    }
    temp->owner = getpid();
    memcpy(temp->path, path, strlen(path) + 1);
    temp->dev = st.st_dev;
    temp->ino = st.st_ino;
    temp->next = hd_temps;
    hd_temps = temp;
    jash_heredoc_track_lifetime();
    return fd;
}

static void hd_cleanup_temps(void) {
    if (!hd_temps) return;
    /* ECHILD says nothing about grandchildren. Once execution might have
     * passed paths to descendants, retain files even after an explicit wait. */
    int keep = jash_heredoc_may_escape();
    pid_t owner = getpid();
    while (hd_temps) {
        hd_temp_t* temp = hd_temps;
        hd_temps = temp->next;
        struct stat st;
        if (!keep && temp->owner == owner && lstat(temp->path, &st) == 0 &&
            st.st_dev == temp->dev && st.st_ino == temp->ino &&
            unlink(temp->path) != 0) {
            fprintf(stderr, "jash: heredoc: cleanup %s: %s\n", temp->path, strerror(errno));
        }
        free(temp);
    }
}

int jash_run_multiline(jash_state_t* state, const char* sp) {
    /* Registry is stack-disciplined so `source` re-entering this function
     * (see the reentrancy note below) frees only what THIS call registered. */
    int _hd_mark = jash_heredoc_registry_mark();
    /* The processing buffer must be reentrancy-safe: `source FILE`
     * (jash_b_source) re-enters jash_run_multiline while THIS invocation is
     * still walking the buffer through `rp`. A single shared static buffer
     * let the inner call clobber the outer walk, leaking buffer fragments as
     * bogus commands (e.g. "export: command not found", nondeterministic).
     * Keep the static buffer as a no-realloc fast path for the common
     * non-reentrant case, but fall back to a per-call heap buffer when the
     * static one is already in use. F E R M I ∞ H A R T */
    static char*  processed_buf;
    static size_t processed_cap;      /* JINV-081: capacidade REAL, que cresce */
    static int    processed_buf_busy;
    char*  heap_buf = NULL;
    char*  pbuf;
    size_t pcap;
    int    use_static;
    if (!processed_buf_busy) {
        if (!processed_buf) {
            processed_buf = (char*)malloc(JASH_SCRIPT_PROC_CAP);
            if (!processed_buf) return 127;  /* OOM */
            processed_cap = JASH_SCRIPT_PROC_CAP;
        }
        processed_buf_busy = 1;
        pbuf = processed_buf; pcap = processed_cap; use_static = 1;
    } else {
        heap_buf = (char*)malloc(JASH_SCRIPT_PROC_CAP);
        if (!heap_buf) return 127;  /* OOM */
        pbuf = heap_buf; pcap = JASH_SCRIPT_PROC_CAP; use_static = 0;
    }
    /* JINV-081: os acumuladores de enunciado e de linha física (antes
     * `char sline[4096]` e `char ppeek[4096]`). Declarados AQUI, antes do
     * pré-processamento, para que a recusa por falta de memória possa saltar
     * direto para a porta única de saída sem pular inicialização. */
    jash_accum_t sacc, pacc;
    jash_accum_init(&sacc);
    jash_accum_init(&pacc);
    int accum_oom = 0;
    int syntax_eof = 0;   /* JINV-094: operador pendente no FIM do programa */
/* JINV-081 — `pbuf` guardava o script PRÉ-PROCESSADO e parava em
 * JASH_SCRIPT_PROC_CAP (128 KiB) em silêncio: `while (*rp && pb < CAP - 1)`.
 * Acima disso a cauda do script simplesmente não existia. Penhasco medido com
 * um `for` de recheio: 125.623 B roda igual ao bash; 139.823 B não. Dos 19
 * `configure` de autoconf medidos, 11 são MAIORES que 128 KiB — libtool
 * (511 KB), bash-5.2 (650 KB), ncurses/test (674 KB).
 *
 * JASH_LINE_CAP é o teto de uma LINHA LÓGICA (core/limits.h). Usá-lo como
 * teto do SCRIPT INTEIRO era um erro de categoria: um script não é uma linha.
 * Ele continua sendo o tamanho INICIAL (nenhuma alocação a mais no caso
 * comum); o que deixa de existir é o teto. */
#define PBUF_ROOM(extra)                                                       \
    ((size_t)pb + (size_t)(extra) + 1 <= pcap                                  \
       ? 1                                                                     \
       : (jash_buf_grow(&pbuf, &pcap, (size_t)pb + (size_t)(extra) + 1)        \
            ? ((use_static ? (void)(processed_buf = pbuf, processed_cap = pcap)\
                           : (void)(heap_buf = pbuf)), 1)                      \
            : (accum_oom = 1, 0)))
    unsigned pb = 0;
    const char* rp = sp;
    while (*rp && PBUF_ROOM(1)) {
        const char* hd = jash_find_unquoted_heredoc_op(sp, rp);
        /* Here-string `cmd <<< word` (BUG-033): materialize `word` into a temp
         * file and rewrite to `< tmpfile`, reusing the heredoc temp-file path so
         * BOTH redirect engines (builtin_redir + pipeline) — which already handle
         * `< file` — work without per-engine `<<<` support. Quotes are removed at
         * preprocess; `$VAR`/`$(..)`/`$((..))` expansion is DEFERRED via hd_pending
         * (like an unquoted heredoc body), so a var set earlier in the same script
         * still resolves. A fully single-quoted word is literal (no expansion). */
        if (hd && hd[2] == '<' && (hd == rp || hd[-1] == ' ' || hd[-1] == '\t')) {
            while (rp < hd && PBUF_ROOM(1)) pbuf[pb++] = *rp++;   /* JINV-081 */
            rp += 3;  /* skip <<< */
            while (*rp == ' ' || *rp == '\t') rp++;
            /* read ONE shell word (quotes group; ends at unquoted ws/metachar) */
            char hw[2048]; unsigned hwi = 0; char hqc = 0;
            int hs_no_expand = 0;
            if (*rp == '\'') {   /* fully single-quoted ⇒ literal (detect span) */
                unsigned z = 1; while (rp[z] && rp[z] != '\'') z++;
                if (rp[z] == '\'' && (rp[z+1] == '\0' || rp[z+1] == ' ' || rp[z+1] == '\t' ||
                                      rp[z+1] == '\n' || rp[z+1] == ';' || rp[z+1] == '|' ||
                                      rp[z+1] == '&' || rp[z+1] == '>'))
                    hs_no_expand = 1;
            }
            while (*rp && hwi < sizeof(hw) - 1) {
                char c = *rp;
                if (hqc) { hw[hwi++] = c; if (c == hqc) hqc = 0; rp++; continue; }
                if (c == '\'' || c == '"') { hqc = c; hw[hwi++] = c; rp++; continue; }
                if (c == ' ' || c == '\t' || c == '\n' || c == ';' || c == '|' ||
                    c == '&' || c == '<' || c == '>' || c == '(' || c == ')') break;
                if (c == '\\' && rp[1]) { hw[hwi++] = c; rp++; if (hwi < sizeof(hw)-1) hw[hwi++] = *rp++; continue; }
                hw[hwi++] = c; rp++;
            }
            hw[hwi] = '\0';
            (void)hs_no_expand;   /* expansion respects single quotes; unquote runs in the deferred pass */
            /* trailing rest-of-line (other args/redirs/pipe) after the word */
            char hstrail[512]; unsigned hti = 0;
            while (*rp && *rp != '\n' && hti + 1 < sizeof(hstrail)) hstrail[hti++] = *rp++;
            hstrail[hti] = '\0';
            if (*rp == '\n') rp++;
            /* Write the RAW word + newline. Expansion AND quote removal are deferred
             * to jash_heredoc_expand_pending (is_herestring path) so that (a) vars
             * resolve against the env at execution time, and (b) expansion runs
             * BEFORE quote removal — otherwise unquoting the raw word would strip
             * the inner quotes of a `$( ... "x" ... )` substitution. */
            char tmpname[256]; snprintf(tmpname, sizeof tmpname, "%s/jash_heredoc_XXXXXX", jash_tmpdir());
            int tmpfd = hd_mkstemp(tmpname);
            if (tmpfd < 0) {
                fprintf(stderr, "jash: here-string: temporary file: %s\n", strerror(errno));
                state->last_status.code = 2;
                goto multiline_done;
            }
            if (tmpfd >= 0) {
                fcntl(tmpfd, F_SETFD, FD_CLOEXEC);
                WRITE_IGNORE(tmpfd, hw, hwi);
                WRITE_IGNORE(tmpfd, "\n", 1);   /* here-strings get a trailing newline */
                close(tmpfd);
                /* Register the raw word + newline for re-entrant expansion. */
                { char hwn[2049]; unsigned k = 0;
                  for (; k < hwi && k < sizeof(hwn) - 2; k++) hwn[k] = hw[k];
                  hwn[k++] = '\n'; hwn[k] = '\0';
                  jash_heredoc_register(tmpname, hwn, 1); }
                const char* pfx = " < ";
                char quoted[4 * sizeof(tmpname) + 3];
                size_t qlen = jash_heredoc_quote_path(tmpname, quoted, sizeof quoted);
                /* JINV-081: reserva o bloco inteiro de uma vez — prefixo,
                 * nome do temporario, cauda da linha e o '\n'. */
                if (PBUF_ROOM(strlen(pfx) + qlen + strlen(hstrail) + 1)) {
                    for (unsigned k = 0; pfx[k]; k++)     pbuf[pb++] = pfx[k];
                    for (unsigned k = 0; quoted[k]; k++)  pbuf[pb++] = quoted[k];
                    for (unsigned k = 0; hstrail[k]; k++) pbuf[pb++] = hstrail[k];
                    pbuf[pb++] = '\n';
                }
            }
            continue;
        }
        if (hd && hd[2] != '(' && hd[2] != '<' && (hd == rp || hd[-1] == ' ' || hd[-1] == '\t')) {
            while (rp < hd && PBUF_ROOM(1))   /* JINV-081 */
                pbuf[pb++] = *rp++;
            /* ══ JINV-095 — UMA LINHA PODE ABRIR VÁRIOS HEREDOCS ═══════════
             *
             * POSIX XCU 2.7: os operadores `<<` de uma linha lógica são
             * coletados NA ORDEM em que aparecem, e os corpos vêm todos
             * DEPOIS da nova-linha, na mesma ordem. `cat <<A <<B` e
             * `cat <<A; cat <<B` são as duas formas.
             *
             * O tratador aqui só sabia de UM. O que vinha depois do
             * delimitador era guardado como texto opaco (`trailing`) e
             * reemitido cru — então um segundo `<<B` chegava ao avaliador
             * como o texto literal `<<B`, sem corpo, e as linhas do corpo
             * dele voltavam ao parser como COMANDOS:
             *
             *     cat <<A; cat <<B      bash: `alpha` `beta`
             *     alpha                 jash: `alpha` + `beta: command not found`
             *     A
             *     beta
             *     B
             *
             * É a família do JINV-016/020/022/092: DADO VIRANDO COMANDO.
             *
             * O `trailing` tinha ainda um TETO MUDO de 256 bytes, e o corte
             * deixava `rp` PARADO NO MEIO DA LINHA — a cauda da linha virava
             * corpo do heredoc e o corpo virava comando. Medido: com 400
             * bytes depois do delimitador a saída sai fora de ordem e
             * partida ao meio (`cat <<EOF ; echo <400 bytes>`). É o
             * JINV-029 (`audits/ACHADO-buffers-fixos.md`) sobrevivendo neste
             * sítio. A varredura por linha abaixo não tem esse teto: o texto
             * vai direto para `pbuf`, que cresce (JINV-081).
             *
             * FASE 1 varre a LINHA: copia o texto e, para cada `<<`, cria o
             * temporário e emite ` < <temporário>` no lugar, guardando o
             * pedido. FASE 2 consome os corpos, na ordem. Com dois na mesma
             * palavra (`cat <<A <<B`) sai `cat < tmpA < tmpB` e vence o
             * ÚLTIMO — que é exatamente o que o bash faz. */
            {
            hd_req_t hdq[JASH_HD_PER_LINE];
            int nhdq = 0;
            /* ── FASE 1: a linha ─────────────────────────────────────── */
            /* `at_first`: a PRIMEIRA posição desta varredura é exatamente o
             * `hd` que o laço de fora já aceitou — inclusive quando o `<<`
             * vem COLADO no que vem antes (`cat<<EOF`, `echo a;cat<<EOF`, o
             * `0<<EOF` de fd explícito), formas que a guarda de branco
             * recusaria. Sem esta lembrança a varredura por linha seria MAIS
             * ESTRITA que o tratador que ela substitui, e `cat<<EOF` — forma
             * legítima e comum — passaria a devolver o corpo ao parser como
             * COMANDO. Medido antes de nascer: as três formas viravam
             * `cat<<EOF: command not found` mais o corpo executado. */
            int at_first = 1;
            while (*rp && *rp != '\n' && !accum_oom) {
                int is_op = (rp[0] == '<' && rp[1] == '<' &&
                             rp[2] != '<' && rp[2] != '(' &&
                             (at_first || rp == sp ||
                              rp[-1] == ' ' || rp[-1] == '\t') &&
                             jash_pos_is_unquoted(sp, rp));
                at_first = 0;
                if (!is_op || nhdq >= JASH_HD_PER_LINE) {
                    if (!PBUF_ROOM(1)) break;
                    pbuf[pb++] = *rp++;
                    continue;
                }
                rp += 2;                       /* JINV-062: sempre anda */
                hd_req_t* q = &hdq[nhdq];
                q->indented = 0;
                if (*rp == '-') { q->indented = 1; rp++; }
                while (*rp == ' ' || *rp == '\t') rp++;
                q->no_expand = 0;
                /* The delimiter is a single shell WORD — it ends at whitespace
                 * OR any unquoted metacharacter, not just whitespace/newline.
                 * Without the metachar stop, `cat <<A; cat <<B` read "A;" as
                 * the delimiter (no space between A and ;), never matched the
                 * real closing line "A", and the heredoc body capture ran off
                 * into the rest of the script — found via difftest.sh battery
                 * round 3, manifested as a hang / "cat: cat: No such file or
                 * directory".
                 *
                 * JINV-092: a leitura da palavra (fronteira + REMOÇÃO DE
                 * CITAÇÃO) mora em `jash_heredoc_delim`, contrato em
                 * parser/heredoc.h — a mesma que o blindador de corpo usa.
                 * Aqui havia uma segunda cópia que só reconhecia `'…'`/`"…"`
                 * inteiriças: `<<\EOF` ficava guardado como `\EOF`, a linha
                 * `EOF` nunca casava, e o laço de captura corria até o FIM DA
                 * ENTRADA — devolvendo o corpo inteiro ao parser como COMANDO.
                 * É o estilo da casa do autoconf (`<<\_ACEOF`) e a causa
                 * medida da classe `missing fi`. */
                rp = jash_heredoc_delim(rp, q->delim, sizeof(q->delim),
                                        &q->no_expand);
                /* O que vem DEPOIS do delimitador e ainda nesta linha
                 * (o " | tr a-z A-Z" de `cat <<EOF | tr a-z A-Z`) é
                 * continuação real do comando — não corpo, que só começa na
                 * linha seguinte. Antes era engolido aqui e o cano sumia;
                 * agora a varredura simplesmente continua copiando a linha,
                 * então ele chega inteiro ao avaliador — sem o teto de 256
                 * bytes que o buffer `trailing` impunha. */
                snprintf(q->name, sizeof(q->name), "%s/jash_heredoc_XXXXXX",
                         jash_tmpdir());
                q->fd = hd_mkstemp(q->name);
                if (q->fd < 0) {
                    fprintf(stderr, "jash: heredoc: temporary file: %s\n", strerror(errno));
                    for (int qi = 0; qi < nhdq; qi++) close(hdq[qi].fd);
                    state->last_status.code = 2;
                    goto multiline_done;
                }
                fcntl(q->fd, F_SETFD, FD_CLOEXEC);
                const char* pfx = " < ";
                char quoted[4 * sizeof(q->name) + 3];
                size_t qlen = jash_heredoc_quote_path(q->name, quoted, sizeof quoted);
                if (!PBUF_ROOM(strlen(pfx) + qlen)) {
                    close(q->fd); q->fd = -1; break;
                }
                for (unsigned k = 0; pfx[k]; k++)     pbuf[pb++] = pfx[k];
                for (unsigned k = 0; quoted[k]; k++)  pbuf[pb++] = quoted[k];
                nhdq++;
            }
            if (*rp == '\n') rp++;
            if (PBUF_ROOM(1)) pbuf[pb++] = '\n';
            /* ── FASE 2: os corpos, na ordem ─────────────────────────── */
            for (int qi = 0; qi < nhdq; qi++) {
            hd_req_t* q = &hdq[qi];
            const int indented = q->indented;
            const int no_expand = q->no_expand;
            const char* delim = q->delim;
            /* JINV-022: o corpo do heredoc cabia em 4096 bytes FIXOS. Acima
             * disso o laco abaixo parava, `rp` ficava PARADO NO MEIO DO CORPO,
             * e todo o resto — texto que o autor escreveu como DADO — voltava
             * ao parser e era EXECUTADO COMO COMANDO, linha a linha.
             *
             * Medido: um corpo de 5.4 KB era cortado em 4095 bytes e os ~1.3 KB
             * restantes viravam comandos ("linha 0093 de teste" -> `ha:
             * command not found`). Sem nenhum aviso. Foi assim que um relatorio
             * em markdown, passado num heredoc, teve o proprio texto executado.
             *
             * Mesma classe do JINV-016 e do JINV-020 — dado virando comando —
             * so que aqui o gatilho e o TAMANHO, entao aparece justamente
             * quando o conteudo e grande demais para alguem revisar.
             *
             * Correcao: buffer que cresce (idioma do JINV-007). Se a memoria
             * faltar, o corpo restante e CONSUMIDO ate o delimitador antes de
             * desistir — nunca devolvido ao parser como comando. */
            size_t ccap = 8192;
            char* content = (char*)malloc(ccap);
            int hd_oom = 0;
            if (!content) { ccap = 0; hd_oom = 1; }
            unsigned ci = 0;
            while (*rp) {
                const char* check = rp;
                if (indented) { while (*check == ' ' || *check == '\t') check++; }
                if (strncmp(check, delim, strlen(delim)) == 0 &&
                    (check[strlen(delim)] == '\n' || check[strlen(delim)] == '\0')) {
                    while (*rp && *rp != '\n') rp++;
                    if (*rp == '\n') rp++;
                    break;
                }
                /* BUG-034: `<<-EOF` strips leading TABs (only tabs, not spaces)
                 * from each body line, matching POSIX/bash. The delimiter match
                 * above already ignores indentation; here we strip the body. */
                if (indented) { while (*rp == '\t') rp++; }
                /* JINV-022: consome a linha inteira SEMPRE. Quando ha memoria,
                 * guarda; quando nao ha, apenas descarta — o ponteiro segue
                 * andando ate o delimitador, entao o corpo jamais escapa para
                 * o parser. */
                while (*rp && *rp != '\n') {
                    if (!hd_oom && ci + 2 >= ccap) {
                        size_t ncap = ccap * 2;
                        char* n2 = (char*)realloc(content, ncap);
                        if (!n2) { hd_oom = 1; }
                        else { content = n2; ccap = ncap; }
                    }
                    if (!hd_oom) content[ci++] = *rp;
                    rp++;
                }
                if (*rp == '\n') {
                    if (!hd_oom && ci + 2 >= ccap) {
                        size_t ncap = ccap * 2;
                        char* n2 = (char*)realloc(content, ncap);
                        if (!n2) { hd_oom = 1; }
                        else { content = n2; ccap = ncap; }
                    }
                    if (!hd_oom) content[ci++] = '\n';
                    rp++;
                } else break;
            }
            if (hd_oom) {
                fputs("jash: heredoc: sem memoria para o corpo — recusando "
                      "(o corpo NAO foi executado)\n", stderr);
                free(content);
                close(q->fd);           /* JINV-095: o temporário já existe */
                continue;               /* JINV-022: nada vira comando */
            }
            content[ci] = '\0';
            /* JINV-095: o temporário e o ` < <nome>` já saíram na FASE 1 — a
             * ordem dos redirects da linha tinha que ser preservada e só o
             * nome é conhecido antes do corpo. Aqui só se escreve o corpo.
             *
             * Write the body verbatim here — $VAR expansion (unquoted
             * delimiter only) happens lazily in jash_heredoc_expand_pending,
             * right before the line that consumes this file executes. */
            WRITE_IGNORE(q->fd, content, ci);
            close(q->fd);
            /* Quoted delimiter (<<'EOF') ⇒ literal body, never expanded, so
             * it is NOT registered. Unquoted ⇒ register raw for re-entrant
             * expansion at execution time (BUG-030). */
            if (!no_expand) jash_heredoc_register(q->name, content, 0);
            free(content);                       /* JINV-022 */
            }   /* FASE 2 */
            }   /* JINV-095 */
        } else {
            pbuf[pb++] = *rp++;
        }
    }
    pbuf[pb] = '\0';
    /* JINV-081: se faltou memória no pré-processamento, NADA roda — nem a
     * parte que coube. Executar meio script com rc 0 é exatamente o defeito
     * que esta onda existe para matar. */
    if (accum_oom) goto multiline_done;

    /* ═══ JINV-094 — o operador no fim da linha pede a linha seguinte ══════
     *
     * Roda AQUI, sobre o programa INTEIRO, e não no avaliador: quando
     * `jash_eval_line` recebe `echo abc |` o corte em linhas lógicas já
     * aconteceu e a nova-linha não existe mais para ser removida. Uma passada
     * só, e ela conserta topo, corpo de bloco, corpo de `$( )` e de crase —
     * todos saem daqui.
     *
     * Depois do pré-processamento de heredoc de propósito: os corpos já foram
     * para arquivos temporários, então um `|` no fim de uma LINHA DE DADO de
     * heredoc não é confundido com estrutura.
     *
     * Reescreve `pbuf` no lugar (a transformação nunca cresce — contrato em
     * parser/quotes.h). */
    {
        jash_join_operator_lines(pbuf, &syntax_eof);
        if (syntax_eof) goto multiline_done;
    }

    rp = pbuf;
/* Garante espaço para mais `extra` bytes em `sline` (NUL incluso) e ressincroniza
 * o ponteiro local depois de um eventual realloc. 0 = sem memória (recusa alta). */
#define SLINE_ROOM(extra)                                                      \
    ((size_t)si + (size_t)(extra) + 1 <= sacc.cap                              \
       ? 1                                                                     \
       : (jash_accum_reserve(&sacc, si, (size_t)si + (size_t)(extra) + 1)      \
            ? (sline = sacc.buf, 1)                                            \
            : (accum_oom = 1, 0)))
#define PPEEK_ROOM(extra)                                                      \
    ((size_t)pi + (size_t)(extra) + 1 <= pacc.cap                              \
       ? 1                                                                     \
       : (jash_accum_reserve(&pacc, pi, (size_t)pi + (size_t)(extra) + 1)      \
            ? (ppeek = pacc.buf, 1)                                            \
            : (accum_oom = 1, 0)))
    while (*rp) {
        char* sline = sacc.buf;   /* JINV-081 */
        unsigned si = 0;
        /* Read a logical line: a newline only ends the line when we are NOT
         * inside an open '...' or "..." quote — otherwise the newline is part
         * of the string. POSIX semantics; without this `echo "a<NL>b"` from
         * a multi-line -c string is split into two broken commands.
         * BUG-090: a newline also does NOT end the line inside an open
         * unquoted paren group — `out=$(cat <FILE⏎)` and `(cmd⏎cmd⏎)` are one
         * statement; splitting at the newline ran the continuation lines as
         * commands. Array literals `name=(` are excluded (the `=`-preceded
         * paren) — they keep the dedicated BUG-089 accumulator with its
         * comment stripping. Comments disable counting to end-of-line. */
        int sq = 0, dq = 0;
        int pd = 0, in_cmt = 0;
        /* BUG-115: `${` inside "…" restarts quoting (the pattern of
         * ${w#"'"} has its own quote pairs); a flat dq toggle inverted
         * parity and the rest of the SCRIPT was swallowed. Push dq on
         * `${`-in-dq, restore at the matching `}`.
         * BUG-121b: an open `…` backtick span also keeps the line going.
         * BUG-154: `$(` inside "…" ALSO restarts quoting (POSIX 2.2.3) —
         * same class as BUG-115's ${ and eval.c's BUG-153. In
         * `v="$(grep '"' f)"` the flat dq toggle read the quote inside
         * the single-quoted pattern as the CLOSE of the value's dq,
         * parity inverted, the newline stopped ending lines, and the
         * next script line was swallowed into the assignment. Push dq
         * on `$(`-in-dq, restore at the matching `)`. */
        int bd = 0; char bdq[16];
        char pdq[16] = {0};
        int bt = 0;
        char pchar = '\n';
        /* JINV-081: `SLINE_ROOM(2)` porque nenhuma iteração deste laço escreve
         * mais de 2 bytes (as três formas `$…` de 2 caracteres abaixo). */
        while (*rp && SLINE_ROOM(2)) {
            char ch = *rp;
            if (ch == '\n') {
                if (!sq && !dq && pd == 0 && !bt) { rp++; break; }
                in_cmt = 0;   /* comment ends at the newline */
            }
            if (!sq && !in_cmt && dq && ch == '$' && rp[1] == '{') {
                if (bd < (int)sizeof(bdq)) bdq[bd] = 1;
                bd++; dq = 0;
                sline[si++] = ch;
                if (si < sacc.cap - 1) sline[si++] = rp[1];   /* JINV-081 */
                pchar = '{'; rp += 2;
                continue;
            }
            if (!sq && !in_cmt && dq && ch == '$' && rp[1] == '(') {
                if (pd < (int)sizeof(pdq)) pdq[pd] = 1;   /* BUG-154 */
                pd++; dq = 0;
                sline[si++] = ch;
                if (si < sacc.cap - 1) sline[si++] = rp[1];   /* JINV-081 */
                pchar = '('; rp += 2;
                continue;
            }
            if (!sq && !in_cmt && ch == '\\' && rp[1]) {
                /* POSIX line continuation: a backslash immediately before a
                 * newline (outside single quotes) removes BOTH — joining this
                 * line with the next. Applies unquoted and inside double quotes,
                 * matching bash. Without this, `echo one \<NL>two` kept a literal
                 * backslash-newline and never joined. NOT inside a comment —
                 * `# foo \` does not continue (bash). */
                if (rp[1] == '\n') { rp += 2; continue; }
                sline[si++] = ch;
                pchar = *rp;
                rp++;
                if (si < sacc.cap - 1) { pchar = *rp; sline[si++] = *rp++; }  /* JINV-081 */
                continue;
            }
            /* BUG-114: everything in a comment is inert — an apostrophe in
             * `# it's a note` toggled sq, the newline stopped ending lines,
             * and the ENTIRE rest of the script was silently swallowed. */
            if (!dq && !in_cmt && ch == '\'') sq = !sq;
            else if (!sq && !in_cmt && ch == '"') dq = !dq;
            else if (!sq && !dq && !in_cmt) {
                if (ch == '#' && (pchar == ' ' || pchar == '\t' ||
                                  pchar == '\n' || pchar == ';' ||
                                  pchar == '(' || pchar == '|' || pchar == '&'))
                    in_cmt = 1;
                else if (ch == '`') bt = !bt;   /* BUG-121b: backtick span */
                /* BUG-121c: the `=`-guard (array literals go to the BUG-089
                 * accumulator) applies only at TOP level — inside an open
                 * $() a nested `arr=(` must count, or its closing paren
                 * unbalanced the cmdsub and truncated the statement. */
                else if (ch == '(' && (pd > 0 || pchar != '=')) pd++;
                else if (ch == ')' && pd > 0) {
                    pd--;
                    if (pd < (int)sizeof(pdq) && pdq[pd]) {
                        pdq[pd] = 0; dq = 1;  /* BUG-154: cmdsub closed — resume dq */
                    }
                }
                else if (ch == '{' && bd > 0) {
                    /* nested brace inside an open ${…} */
                    if (bd < (int)sizeof(bdq)) bdq[bd] = 0;
                    bd++;
                } else if (ch == '}' && bd > 0) {
                    bd--;
                    if (bd < (int)sizeof(bdq)) dq = bdq[bd];  /* BUG-115 */
                }
            }
            pchar = ch;
            sline[si++] = ch;
            rp++;
        }
        sline[si] = '\0';
        if (accum_oom) break;   /* JINV-081: recusa alta, nunca corte mudo */

        /* Backslash continuation: odd trailing backslash count = join.
         * (Only meaningful outside of quotes; the loop above handles the
         * inside-quote case naturally by NOT terminating on the newline.)
         * BUG-114: not when the line ends inside a COMMENT — `# note \`
         * must not swallow the next line. */
        while (si > 0 && sline[si - 1] == '\\' && !sq && !dq && !in_cmt) {
            int nbs = 0;
            int k = (int)si - 1;
            while (k >= 0 && sline[k] == '\\') { nbs++; k--; }
            if (nbs % 2 == 0) break;
            si--;
            sline[si] = '\0';
            while (*rp && *rp != '\n' && SLINE_ROOM(1))   /* JINV-081 */
                sline[si++] = *rp++;
            sline[si] = '\0';
            if (*rp == '\n') rp++;
        }
        if (accum_oom) break;   /* JINV-081 */

        if (si == 0) continue;

        const char* trimmed = sline;
        while (*trimmed == ' ' || *trimmed == '\t') trimmed++;

        /* Function definition without the 'function' keyword: name() { ... }
         * The keyword splitter below only knows function/if/for/while/case/select,
         * so a multi-line `name() {` body would be split line-by-line and break.
         * Accumulate it by brace depth instead. F E R M I ∞ H A R T */
        {
            const char* q = trimmed;
            int looks_func = 0;
            /* bash: function names are WORDs, not POSIX identifiers —
             * `cmake-build() {` is valid (see parse_function_def, eval.c). */
            if (isalpha((unsigned char)*q) || *q == '_') {
                while (isalnum((unsigned char)*q) || *q == '_' ||
                       *q == '-' || *q == '.' || *q == '+') q++;
                while (*q == ' ' || *q == '\t') q++;
                if (q[0] == '(' && q[1] == ')') {
                    q += 2;
                    while (*q == ' ' || *q == '\t') q++;
                    if (*q == '{' || *q == '\0') looks_func = 1;
                }
            }
            if (looks_func) {
                int bdepth = 0, seen = 0;
                for (unsigned k = 0; k < si; k++) {
                    if (sline[k] == '{') { bdepth++; seen = 1; }
                    else if (sline[k] == '}') bdepth--;
                }
                while (*rp && (bdepth > 0 || !seen) && SLINE_ROOM(1)) {  /* JINV-081 */
                    sline[si++] = '\n';
                    while (*rp && *rp != '\n' && SLINE_ROOM(1)) {        /* JINV-081 */
                        char ch = *rp++;
                        if (ch == '{') { bdepth++; seen = 1; }
                        else if (ch == '}') bdepth--;
                        sline[si++] = ch;
                    }
                    sline[si] = '\0';
                    if (*rp == '\n') rp++;
                }
                if (accum_oom) break;   /* JINV-081 */
                sline[si] = '\0';
                jash_eval_line(state, jash_highlighted_stdout_writer, 0, sline);
                if (state->exit_requested) break;
                continue;
            }
        }

        /* BUG-089: multi-line array literal — `a=(x⏎y z⏎)` was split at the
         * first newline, so the continuation lines ran as commands and the
         * array came out empty. If this line opens an assignment paren
         * (`name=(` / `name+=(`) that does not close on the same line
         * (quote-aware balance), accumulate lines until it balances; the
         * compound-assignment parser handles embedded newlines. */
        {
            int pbal = 0; char aqc = 0; int saw_assign_paren = 0;
            const char* cutpos = NULL; char prevch = ' ';
            for (const char* q = trimmed; *q; q++) {
                char c = *q;
                if (aqc) {
                    if (c == '\\' && aqc == '"' && q[1]) { q++; prevch = c; continue; }
                    if (c == aqc) aqc = 0;
                    prevch = c; continue;
                }
                if (c == '\\' && q[1]) { q++; prevch = c; continue; }
                if (c == '\'' || c == '"') { aqc = c; prevch = c; continue; }
                /* Unquoted word-start # comments out the rest of THIS line —
                 * stop counting and remember where to truncate. */
                if (c == '#' && (prevch == ' ' || prevch == '\t' || prevch == '(')) {
                    cutpos = q; break;
                }
                if (c == '(') {
                    if (q > trimmed && q[-1] == '=') saw_assign_paren = 1;
                    pbal++;
                } else if (c == ')') { if (pbal > 0) pbal--; }
                prevch = c;
            }
            if (saw_assign_paren && pbal > 0) {
                if (cutpos) {
                    si = (unsigned)(cutpos - sline);
                    while (si > 0 && (sline[si-1] == ' ' || sline[si-1] == '\t')) si--;
                    sline[si] = '\0';
                }
                while (*rp && pbal > 0 && SLINE_ROOM(1)) {          /* JINV-081 */
                    sline[si++] = '\n';
                    char q2 = 0, pv2 = ' '; int skipping = 0;
                    while (*rp && *rp != '\n' && SLINE_ROOM(1)) {   /* JINV-081 */
                        char ch = *rp++;
                        if (skipping) continue;   /* drop commented tail */
                        if (q2) { if (ch == q2) q2 = 0; }
                        else if (ch == '\'' || ch == '"') q2 = ch;
                        else if (ch == '#' && (pv2 == ' ' || pv2 == '\t')) { skipping = 1; continue; }
                        else if (ch == '(') pbal++;
                        else if (ch == ')') pbal--;
                        sline[si++] = ch;
                        pv2 = ch;
                    }
                    sline[si] = '\0';
                    if (*rp == '\n') rp++;
                }
                if (accum_oom) break;   /* JINV-081 */
                jash_eval_line(state, jash_highlighted_stdout_writer, 0, sline);
                if (state->exit_requested) break;
                continue;
            }
        }

        int need_accumulate =
            (strncmp(trimmed, "case ", 5) == 0 || strncmp(trimmed, "case\t", 5) == 0 ||
             strncmp(trimmed, "for ", 4) == 0 || strncmp(trimmed, "for\t", 4) == 0 ||
             (strncmp(trimmed, "for((", 5) == 0) ||
             strncmp(trimmed, "while ", 6) == 0 || strncmp(trimmed, "while\t", 6) == 0 ||
             strncmp(trimmed, "until ", 6) == 0 || strncmp(trimmed, "until\t", 6) == 0 ||
             strncmp(trimmed, "if ", 3) == 0 || strncmp(trimmed, "if\t", 3) == 0 ||
             strncmp(trimmed, "select ", 7) == 0 || strncmp(trimmed, "select\t", 7) == 0 ||
             strncmp(trimmed, "function ", 9) == 0 || strncmp(trimmed, "function\t", 9) == 0 ||
             /* ═══ JINV-084 — o grupo de chaves era o composto SEM DONO ══════
              *
              * `{` não estava nesta lista, então uma linha que abria um grupo
              * NUNCA entrava no acumulador: ela era entregue sozinha ao
              * avaliador, que não tem o que fazer com um `{` solto e o executa
              * como o NOME DE UM PROGRAMA. Medido, com o bash de oráculo:
              *
              *     {                    bash -> hi        jash -> `{: command not found`
              *     echo hi                                       `hi`
              *     }                                             `}: command not found`
              *
              * — e as duas linhas de erro saem ANTES e DEPOIS do corpo, que
              * roda solto no shell de fora. Impacto medido: 5 dos 10
              * `configure` ainda travados imprimem `}: command not found` bem
              * na frente do erro que os mata; é o `as_fn_*` do autoconf, que
              * escreve o corpo da função como grupo multi-linha.
              *
              * `{` entra aqui como qualquer outra abertura, com `}` de
              * fechador. A forma `nome() {` já tinha dono (o ramo `looks_func`
              * acima, que roda ANTES deste) e não muda de caminho.
              *
              * POSIX 2.9: `{` só é palavra reservada seguida de BRANCO — então
              * `{echo` continua sendo o comando `{echo` (no bash também) e
              * `{a,b}` continua sendo expansão de chaves. */
             (trimmed[0] == '{' && (trimmed[1] == ' ' || trimmed[1] == '\t' ||
                                    trimmed[1] == '\n' || trimmed[1] == '\0')));

        if (need_accumulate) {
            const char* close_kw = NULL;
            if (strncmp(trimmed, "case ", 5) == 0 || strncmp(trimmed, "case\t", 5) == 0)
                close_kw = "esac";
            else if (strncmp(trimmed, "if ", 3) == 0 || strncmp(trimmed, "if\t", 3) == 0)
                close_kw = "fi";
            else if (trimmed[0] == '{')          /* JINV-084 */
                close_kw = "}";
            else
                close_kw = "done";

            /* BUG-087: a ONE-LINE compound (`if …; fi`, `case … esac`,
             * `for …; done`) already closes on this line. Entering the
             * accumulator here glued EVERY following script line into this
             * statement (the close-keyword scan below only looks at line
             * starts), and the compound evaluator then discarded the glued
             * tail — silently truncating the rest of the script with rc=0.
             * Count same-type opens/closes on the current line; if the
             * balance reaches zero the statement is complete — evaluate it
             * alone and do NOT accumulate. */
            /* JINV-083: esta contagem tinha fronteira de palavra (`at_word`)
             * mas NAO era consciente de aspas — e uma nao substitui a outra.
             * `for i in 1; do echo "ask for help"; done` tem um `for ` CITADO
             * que e uma palavra perfeitamente delimitada: `bal` subia para 2,
             * o `done` real baixava para 1, o enunciado nao fechava, e o
             * acumulador colava TODO o resto do script dentro dele — o mesmo
             * estrago que o BUG-087 acima descreve, so que por aspas. Agora
             * usa a leitura unica da casa (parser/script.h). */
            {
                int bal = 0, one_line_complete = 0;
                jash_kw_t kw[5];
                size_t nk = 0;
                /* JINV-084: para `{`/`}` a contagem já existe pronta e é a
                 * MESMA leitura — `jash_find_brace_close` (parser/script.h),
                 * que é `jash_find_keyword_set` com o par de chaves. Montar
                 * aqui um segundo par de `jash_kw_t` seria escrever a nona
                 * varredura da casa para reusar a oitava. */
                if (strcmp(close_kw, "}") == 0) {
                    if (jash_find_brace_close(trimmed + 1)) {
                        jash_eval_line(state, jash_highlighted_stdout_writer, 0, sline);
                        if (state->exit_requested) break;
                        continue;
                    }
                    goto brace_accumulate;   /* não fecha nesta linha: acumula */
                }
#define JASH_KW_PUSH(W, E) do { kw[nk].word = (W); kw[nk].endset = (E); \
                                kw[nk].cmdpos = 1; nk++; } while (0)
                if (strcmp(close_kw, "fi") == 0) {
                    JASH_KW_PUSH("if",     JASH_KW_END_OPEN);
                } else if (strcmp(close_kw, "esac") == 0) {
                    JASH_KW_PUSH("case",   JASH_KW_END_OPEN);
                } else {
                    JASH_KW_PUSH("for",    JASH_KW_END_OPEN);
                    JASH_KW_PUSH("while",  JASH_KW_END_OPEN);
                    JASH_KW_PUSH("until",  JASH_KW_END_OPEN);
                    JASH_KW_PUSH("select", JASH_KW_END_OPEN);
                }
                size_t closer = nk;
                JASH_KW_PUSH(close_kw,     JASH_KW_END_CLOSE);
#undef JASH_KW_PUSH

                const char* lp = trimmed;
                while (*lp) {
                    size_t which = 0;
                    const char* after = 0;
                    const char* hit = jash_find_keyword_set(lp, kw, nk, &which, &after);
                    if (!hit) break;
                    if (which == closer) {
                        bal--;
                        if (bal <= 0) { one_line_complete = 1; break; }
                    } else {
                        bal++;
                    }
                    lp = after;
                }
                if (one_line_complete) {
                    jash_eval_line(state, jash_highlighted_stdout_writer, 0, sline);
                    if (state->exit_requested) break;
                    continue;
                }
            }

brace_accumulate:   /* JINV-084 */
            ;
            /* JINV-084: onde o `{` mora dentro de `sline`, como OFFSET — ver a
             * nota na checagem autoritativa no fim do laço. */
            const unsigned brace_off = (unsigned)(trimmed - sline);
            int depth = 1;
            while (*rp && depth > 0) {
                /* JINV-081: aqui morava o clamp mudo
                 *     if (si >= sizeof(sline)-1) { sline[sizeof(sline)-1]=0; break; }
                 * — o enunciado saía sem a cauda e o avaliador reclamava de um
                 * `done`/`fi` que o autor TINHA escrito. Agora o buffer cresce;
                 * só falta de memória interrompe, e ALTO. */
                if (!SLINE_ROOM(1)) break;
                const char* peek = rp;
                char* ppeek = pacc.buf;   /* JINV-081: era `char ppeek[4096]` */
                unsigned pi = 0;
                while (*peek && *peek != '\n' && PPEEK_ROOM(1))
                    ppeek[pi++] = *peek++;
                if (accum_oom) break;
                ppeek[pi] = '\0';

                const char* pt = ppeek;
                while (*pt == ' ' || *pt == '\t') pt++;
                /* Only count depth for the SAME compound type as the outer one.
                 * E.g. when accumulating an `if` block (close_kw="fi"), a nested
                 * `case`/`esac` must NOT affect depth — only `if`/`fi` pairs do.
                 * Without this, `case` inside an `if` incremented depth but `esac`
                 * never decremented it (wrong close_kw), causing the accumulator
                 * to overshoot and consume the entire rest of the script. */
                int is_nested_open = 0;
                if (strcmp(close_kw, "}") == 0) {
                    /* JINV-084 — para chaves NÃO se conta por início de linha.
                     *
                     * Este laço decide profundidade olhando o começo de cada
                     * linha, e para `{ }` isso é insuficiente por construção: um
                     * `{` que abre no MEIO da linha (`f() {`, o idioma de função
                     * mais comum que existe) não seria contado, mas o `}` que o
                     * fecha começa a linha e SERIA — a contagem fica assimétrica
                     * e o acumulador termina cedo. Medido, com o bash de oráculo:
                     *
                     *     {              bash -> x + y + z
                     *     f() {          jash -> y + z   (o corpo de `f` sumia:
                     *       echo x               o grupo foi cortado no `}` da
                     *     }                      função e `f` ficou indefinida)
                     *     f
                     *     echo y
                     *     }
                     *
                     * A decisão certa é dada pelo MESMO leitor que o avaliador
                     * vai usar depois — `jash_find_brace_close` sobre o texto já
                     * acumulado, logo abaixo. Consultar o mesmo oráculo dos dois
                     * lados é o que garante que acumulador e avaliador não possam
                     * discordar sobre onde o grupo acaba. */
                    is_nested_open = 0;
                } else if (strcmp(close_kw, "fi") == 0) {
                    /* Accumulating if: only count nested if */
                    is_nested_open = (strncmp(pt, "if ", 3) == 0 || strncmp(pt, "if\t", 3) == 0);
                } else if (strcmp(close_kw, "esac") == 0) {
                    /* Accumulating case: only count nested case */
                    is_nested_open = (strncmp(pt, "case ", 5) == 0 || strncmp(pt, "case\t", 5) == 0);
                } else {
                    /* Accumulating done: count for/while/until/select/{ only.
                     * Do NOT count if/case — they have their own close keywords
                     * (fi/esac) and would cause depth overshoot. */
                    is_nested_open = (strncmp(pt, "for ", 4) == 0 || strncmp(pt, "for\t", 4) == 0 ||
                                      strncmp(pt, "for((", 5) == 0 ||
                                      strncmp(pt, "while ", 6) == 0 || strncmp(pt, "while\t", 6) == 0 ||
                                      strncmp(pt, "until ", 6) == 0 || strncmp(pt, "until\t", 6) == 0 ||
                                      strncmp(pt, "select ", 7) == 0 || strncmp(pt, "select\t", 7) == 0);
                }
                if (is_nested_open)
                    depth++;

                /* JINV-084: para `}` quem decide é o leitor único, DEPOIS da
                 * cópia — esta checagem por prefixo de linha fecharia no `}` de
                 * um `f() {` aninhado. */
                if (strcmp(close_kw, "}") != 0 &&
                    strncmp(pt, close_kw, strlen(close_kw)) == 0) {
                    const char* after = pt + strlen(close_kw);
                    if (*after == '\0' || *after == ' ' || *after == '\t' || *after == ';' || *after == '\n')
                        depth--;
                }
                if (strcmp(close_kw, "done") == 0 && strcmp(pt, "}") == 0)
                    depth--;

                /* JINV-081: reserva de uma vez o '\n' + a linha inteira, então
                 * a cópia abaixo não pode parar no meio. */
                if (!SLINE_ROOM((size_t)pi + 1)) break;
                sline[si++] = '\n';
                for (unsigned k = 0; ppeek[k]; k++)
                    sline[si++] = ppeek[k];
                sline[si] = '\0';
                rp = peek;
                if (*rp == '\n') rp++;

                /* JINV-084 — a decisão autoritativa do grupo de chaves.
                 *
                 * Pergunta ao MESMO `jash_find_brace_close` que o avaliador vai
                 * usar se o texto acumulado até aqui já fecha o grupo. Ele conta
                 * aninhamento, respeita aspas e exige posição de comando, então
                 * `f() {`, `${VAR}`, `{a,b}`, `echo }` e `case x in })` entram na
                 * conta exatamente como entrarão do outro lado.
                 *
                 * `brace_off` é OFFSET, não ponteiro: `sline` migra de pilha para
                 * heap quando SLINE_ROOM precisa crescer (JINV-081), e um ponteiro
                 * guardado antes disso apontaria para memória liberada. */
                if (strcmp(close_kw, "}") == 0 &&
                    jash_find_brace_close(sline + brace_off + 1))
                    depth = 0;
            }
            if (accum_oom) break;   /* JINV-081 */
        }

        jash_eval_line(state, jash_highlighted_stdout_writer, 0, sline);
        if (state->exit_requested) break;
    }
#undef SLINE_ROOM
#undef PPEEK_ROOM
#undef PBUF_ROOM
multiline_done:                      /* JINV-081: porta única de saída */
    jash_accum_free(&sacc);
    jash_accum_free(&pacc);
    if (heap_buf) free(heap_buf);
    else processed_buf_busy = 0;
    jash_heredoc_registry_rewind(_hd_mark);
    /* JINV-081: sem memória para o script/enunciado = recusa ALTA. Nunca o par
     * (rc 0, stderr vazio, programa diferente do escrito). */
    if (accum_oom) {
        fputs("jash: sem memoria para o script — RECUSANDO "
              "(nada foi executado pela metade)\n", stderr);
        return 2;
    }
    /* JINV-094: o programa acabou com `|`, `||` ou `&&` pendente. No bash isso
     * é `syntax error: unexpected end of file`, rc 2, e NADA roda — nem a parte
     * antes do operador. Executar o lado esquerdo de um cano que não existe é
     * exatamente a mentira que este invariante existe para matar.
     * `last_status` também recebe o 2: o caminho de ARQUIVO em main() devolve
     * `state->last_status.code` e ignora este retorno. */
    if (syntax_eof) {
        fputs("jash: syntax error: unexpected end of file\n", stderr);
        state->last_status.code = 2;
        return 2;
    }
    return state->last_status.code;
}

/* ULTRA-FAST PATH for trivial `-c CMD`: skip the 355KB state malloc, env seed,
   * job-control setup, cache_init, getcwd, and signal wiring entirely. Eligible
   * commands are pure builtins or pure externals with NO shell expansion needed:
   *   - no $ (no var/cmdsub/arith expansion)
   *   - no quote handling pressure (no ' or " or `)
   *   - no pipes, redirs, subshell, semicolon, &&/||, &
   *   - no compound keyword (if/for/while/etc.)
   * Returns 1 if it handled the command (caller exits with `code`), 0 otherwise.
   * F E R M I ∞ H A R T */
  static int ultra_fast_write_all(int fd, const char* data, size_t len) {
    while (len > 0) {
        ssize_t n = write(fd, data, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        data += (size_t)n;
        len -= (size_t)n;
    }
    return 0;
  }

  /* A one-command, no-Sigil process cannot observe in-memory timeline state
   * after exit. Handle the canonical echo redirect without constructing the
   * full shell state; every richer form falls through to the normal parser. */
  static int ultra_fast_echo_redirect(const char* cmd, int* code) {
    const char* p = cmd;
    while (*p == ' ' || *p == '\t') p++;
    if (strncmp(p, "echo", 4) != 0 ||
        (p[4] != '\0' && p[4] != ' ' && p[4] != '\t')) return 0;
    p += 4;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '-') return 0;

    const char* op = NULL;
    for (const char* q = p; *q; q++) {
        char c = *q;
        if (c == '>') {
            if (op) return 0;
            op = q;
            if (q[1] == '>') q++;
            continue;
        }
        if (c == '$' || c == '\'' || c == '"' || c == '`' ||
            c == '|' || c == ';' || c == '&' || c == '(' ||
            c == ')' || c == '<' || c == '\\' || c == '{' ||
            c == '}' || c == '*' || c == '?' || c == '[' ||
            c == '~' || c == '\n' || c == '#') return 0;
    }
    if (!op) return 0;

    /* An adjacent all-digit word is an IO number, not echo payload. Leave
     * fd handling to the parser; the digit suffix in x2>file is still data. */
    const char* io = op;
    while (io > p && io[-1] >= '0' && io[-1] <= '9') io--;
    if (io < op && (io == p || io[-1] == ' ' || io[-1] == '\t')) return 0;

    int append = op[1] == '>';
    const char* target = op + (append ? 2 : 1);
    while (*target == ' ' || *target == '\t') target++;
    if (!*target) return 0;
    const char* target_end = target;
    while (*target_end && *target_end != ' ' && *target_end != '\t') target_end++;
    const char* tail = target_end;
    while (*tail == ' ' || *tail == '\t') tail++;
    if (*tail) return 0;

    const char* payload_end = op;
    while (payload_end > p &&
           (payload_end[-1] == ' ' || payload_end[-1] == '\t')) payload_end--;
    for (const char* q = p; q < payload_end; q++) {
        if (*q == '\t' || (*q == ' ' && q + 1 < payload_end && q[1] == ' '))
            return 0;
    }

    size_t target_len = (size_t)(target_end - target);
    if (target_len == 0 || target_len >= 1024) return 0;
    char path[1024];
    memcpy(path, target, target_len);
    path[target_len] = '\0';

    int flags = O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC);
    int fd = open(path, flags, 0666);
    if (fd < 0) {
        fprintf(stderr, "jash: %s: %s\n", path, strerror(errno));
        *code = 1;
        return 1;
    }
    size_t payload_len = (size_t)(payload_end - p);
    int failed = 0;
    int write_errno = 0;
    if (payload_len && ultra_fast_write_all(fd, p, payload_len) != 0) {
        failed = 1;
        write_errno = errno;
    }
    if (!failed && ultra_fast_write_all(fd, "\n", 1) != 0) {
        failed = 1;
        write_errno = errno;
    }
    if (close(fd) != 0 && !failed) {
        failed = 1;
        write_errno = errno;
    }
    if (failed) {
        fprintf(stderr, "jash: %s: write failed: %s\n", path,
                strerror(write_errno));
        *code = 1;
    } else {
        *code = 0;
    }
    return 1;
  }

  static int ultra_fast_c_path(const char* cmd, int* code,
                               int allow_redirect) {
    if (!cmd) return 0;
    while (*cmd == ' ' || *cmd == '\t') cmd++;
    if (!*cmd) { *code = 0; return 1; }

    if (allow_redirect && ultra_fast_echo_redirect(cmd, code)) return 1;
    
    /* Fast path for arithmetic: echo $((1+2*3)) */
    if (memcmp(cmd, "echo $", 6) == 0) {
        const char* p = cmd + 6;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '(' && *(p+1) == '(') {
            const char* start = p + 2;
            int depth = 1;
            const char* end = start;
            while (*end) {
                if (*end == '(') { depth++; end++; continue; }
                if (*end == ')') { depth--; end++; if (depth == 0) break; continue; }
                end++;
            }
            if (depth == 0) {
                size_t len = end - start;
                const char* suffix = end + 1;
                while (*suffix == ' ' || *suffix == '\t') suffix++;
                if (*suffix) return 0;
                if (len > 0 && len < 512) {
                    char expr[512];
                    memcpy(expr, start, len);
                    expr[len] = '\0';

                    /* This path has no shell state. Only literal arithmetic is
                     * safe here; variables, arrays and command substitutions
                     * must fall through to the real evaluator. */
                    for (size_t i = 0; i < len; i++) {
                        char c = expr[i];
                        int literal = (c >= '0' && c <= '9') || c == ' ' ||
                            c == '\t' || c == '+' || c == '-' || c == '*' ||
                            c == '/' || c == '%' || c == '(' || c == ')' ||
                            c == '<' || c == '>' || c == '&' || c == '|' ||
                            c == '^' || c == '~' || c == '!' || c == '?' ||
                            c == ':' || c == ',';
                        if (!literal) return 0;
                    }
                    
                    jash_arith_clear_error();
                    long result = jash_eval_arithmetic(expr, NULL);
                    if (jash_arith_error()) return 0;
                    char buf[64];
                    int n = snprintf(buf, sizeof(buf), "%ld\n", result);
                    if (n > 0) { ssize_t _w = write(1, buf, (size_t)n); (void)_w; }
                    *code = 0;
                    return 1;
                }
            }
        }
    }
    
    for (const char* p = cmd; *p; p++) {
        char c = *p;
        if (c == '$' || c == '\'' || c == '"' || c == '`' ||
            c == '|' || c == ';' || c == '&' || c == '(' ||
            c == ')' || c == '<' || c == '>' || c == '\\' ||
            c == '{' || c == '}' || c == '*' || c == '?' ||
            c == '[' || c == '~' || c == '\n' || c == '#')
            return 0;
    }
    char name[256];
    unsigned ni = 0;
    while (*cmd && *cmd != ' ' && *cmd != '\t' && ni + 1 < sizeof(name))
        name[ni++] = *cmd++;
    name[ni] = '\0';
    while (*cmd == ' ' || *cmd == '\t') cmd++;
    /* exit / true / false / : — answerable without any state */
    if (strcmp(name, "exit") == 0 || strcmp(name, ":") == 0 || strcmp(name, "true") == 0) {
        *code = (*cmd >= '0' && *cmd <= '9') ? atoi(cmd) : 0;
        return 1;
    }
    if (strcmp(name, "false") == 0) { *code = 1; return 1; }
    /* echo without flags — write argv2 verbatim + newline.
     * Skip if first arg starts with -, so `echo -n` / `echo -e` fall through.
     *
     * JINV-063 — a TERCEIRA porta, e a mais crua: "verbatim" incluía o espaço
     * SEPARADOR do fim da linha, então `jash -c 'echo x '` imprimia `x `. Aqui
     * a linha já é garantidamente sem aspas (o filtro acima recusa ' " ` \),
     * mas o corte usa a MESMA função das outras duas portas — quem lê o código
     * não deve ter de descobrir por que este lugar seria especial. E o
     * comprimento vai direto no write(): sem cópia, sem buffer. */
    /* JINV-079 — esta via só pode escrever VERBATIM enquanto verbatim for
     * indistinguível do que a porta de argv produziria.
     *
     * O `echo` do bash não copia a linha: ele JUNTA argv[1..n] com UM espaço.
     * Enquanto a cauda tiver só separadores simples, as duas coisas dão o
     * mesmo byte a byte e o atalho é honesto. Assim que aparece um TAB ou uma
     * corrida de dois espaços, não dão: `echo A     B` sai com cinco espaços
     * aqui e com um no bash. A cauda já é garantidamente sem aspas (o filtro
     * de caracteres lá em cima recusa `' " \` `` ` ``), então não há como um
     * desses brancos ser DADO — é sempre estrutura.
     *
     * O conserto NÃO é colapsar aqui: seria a quinta cópia da semântica do
     * `echo`, e a quinta cópia é como esta divergência nasceu. É DESISTIR do
     * atalho e cair no shell completo, onde `jash_b_echo_argv` (a porta única)
     * responde. Custo: só as linhas que o atalho já não sabia responder. */
    if (strcmp(name, "echo") == 0 && *cmd != '-') {
        size_t keep = jash_rtrim_unquoted(cmd);
        int canonical = 1;
        for (size_t i = 0; i < keep; i++) {
            if (cmd[i] == '\t' || (cmd[i] == ' ' && i + 1 < keep && cmd[i + 1] == ' ')) {
                canonical = 0;
                break;
            }
        }
        if (canonical) {
            long w = write(1, cmd, keep); (void)w;
            long n = write(1, "\n", 1); (void)n;
            *code = 0; return 1;
        }
        return 0;   /* cauda não-canônica: quem responde é o builtin de argv */
    }
    /* Anything else: fall through to full init. We deliberately do NOT run
     * externals here — many short names (ls, kill, ps, grep, rm, mv, cp, type,
     * env, ...) are jash builtins that override /bin/<name>, so bypassing the
     * shell would silently change semantics. */
    return 0;
}

/* Wave 7b: turn {"argv":["ls","-la","/a b"]} into a shell-safe command line by
 * SINGLE-QUOTING each element ('\'' for an embedded quote). The agent hands jash
 * an argv array; the args become literal — no word-splitting, globbing, or
 * re-parsing — and routing (builtin vs external) reuses jash's own parser, which
 * is reliable (the quoting hazard was always an *outer* shell, never jash). */
/* JINV-017 — o parser de --exec-json vive em core/serve.c.
 *
 * Aqui existia um SEGUNDO parser (`exec_json_to_cmd`), feito de `strstr`,
 * que só entendia `argv` e aceitava `cwd`, `env` e `stdin` calado — sem
 * aplicar nada — além de emitir `argv[0]` sem citação (a injeção do
 * JINV-016). Ele foi substituído pela porta única `jash_exec_json`, a
 * MESMA que `--serve` usa: um protocolo, uma implementação. */


/* ADR-0037: first-contact agent brief. LLM agents that use jash as their
 * Bash-tool shell usually do not know they are in jash — they never discover
 * recall/expect/rewind and bypass to /bin/bash at the first quoting bump
 * (observed live: the agent that FIXED four jash bugs in one session did all
 * of it through bash wrappers). The only channels guaranteed to reach a
 * model's context are (a) session-start hook injection — see
 * integrations/claude-code/settings.json — and (b) command output. This is
 * channel (b): on the FIRST -c invocation of an agent session, emit a short
 * capability brief on STDERR (never stdout — $(...) captures stay clean).
 * Dedupe: one file, keyed by session id when available (CLAUDE_CODE_SESSION_ID
 * or JASH_SESSION), else a 4h window. JASH_BRIEF=0 silences it entirely.
 * F E R M I ∞ H A R T */
/* ADR-0037: harness-agnostic agent detection. Any LLM harness, not just
 * Claude Code: scan the environment once for the marker families the known
 * agent CLIs export. JASH_AGENT=1 is the documented universal opt-in for
 * harnesses that export nothing recognizable. ~50 strncmp over environ —
 * runs once per -c process, only reached when JASH_BRIEF != 0. */
/* The brief must show ONCE per session — but a single-slot marker was clobbered
 * by concurrent CLAUDE_CODE_SESSION_IDs (the main session plus plugin/child jash
 * invocations, each writing its own sid), which forced a re-brief on nearly every
 * -c call. The marker is now a keyed SET, one "<key>\t<unix_ts>" line per session:
 * each distinct session briefs at most once, and a global rate floor caps briefs
 * to <=1 per BRIEF_FLOOR_SECS even when foreign sids never repeat. Legacy
 * single-line markers (no tab) are treated as stale and ignored. */
#define BRIEF_SET_MAX    16
#define BRIEF_PRUNE_SECS (24L * 3600)
#define BRIEF_FLOOR_SECS 3600

static void jash_agent_first_contact_brief(void) {
    const char* off = getenv("JASH_BRIEF");
    if (off && off[0] == '0') return;
    if (!jash_env_smells_like_agent()) return;

    const char* sid = getenv("CLAUDE_CODE_SESSION_ID");
    if (!sid || !*sid) sid = getenv("JASH_SESSION");
    const char* home = getenv("HOME");
    if (!home || !*home) return;

    char path[600];
    snprintf(path, sizeof path, "%s/.jash", home);
    mkdir(path, 0700);
    snprintf(path, sizeof path, "%s/.jash/agent_brief.seen", home);

    /* This invocation's key: the session id, or "@" for an anonymous session. */
    char key[160];
    snprintf(key, sizeof key, "%s", (sid && *sid) ? sid : "@");
    long now = (long)time(NULL);

    /* Global rate floor: if ANY brief happened within BRIEF_FLOOR_SECS, stay
     * quiet and do NOT touch the file — the floor keeps counting from the last
     * real brief. This alone caps the brief to <=1/floor and kills the spam even
     * when the clobbering foreign sids are fresh on every invocation. */
    struct stat st;
    if (stat(path, &st) == 0 && now - (long)st.st_mtime < BRIEF_FLOOR_SECS)
        return;

    /* Read the keyed set, dropping entries older than the prune window. If this
     * key is already present, this session was briefed — nothing to do. */
    char keys[BRIEF_SET_MAX][160];
    long tss[BRIEF_SET_MAX];
    int  nset = 0;
    FILE* f = fopen(path, "r");
    if (f) {
        char line[256];
        while (nset < BRIEF_SET_MAX && fgets(line, sizeof line, f)) {
            char* nl = strchr(line, '\n'); if (nl) *nl = '\0';
            char* tab = strchr(line, '\t');
            long ts = 0;
            if (tab) { *tab = '\0'; ts = atol(tab + 1); }
            if (!line[0]) continue;
            if (ts <= 0 || now - ts >= BRIEF_PRUNE_SECS) continue;  /* stale/legacy */
            if (strcmp(line, key) == 0) { fclose(f); return; }
            snprintf(keys[nset], sizeof keys[nset], "%s", line);
            tss[nset] = ts;
            nset++;
        }
        fclose(f);
    }

    /* New key + floor elapsed → brief now. */
    fputs(
"[jash] FIRST CONTACT — your shell is jash, an agent-native shell. Not bash.\n",
        stderr);
    /* Canonical superpowers pitch — single source: core/differentiators.c */
    fputs(jash_diff_brief(), stderr);
    fputs(
"Full manual (63 lines, read once): jash --agent-guide\n"
"Multi-line commands, quoting and eval match bash (parity-gated in CI).\n"
"Repeating an unchanged read (cat/grep/ls/head...) returns ONE attested\n"
"[jash memo] line instead of the bytes you already have (JASH_MEMO=0 off).\n"
"Do NOT fall back to /bin/bash: you would lose recording, undo and\n"
"attestation. (Silence this brief: JASH_BRIEF=0)\n",
        stderr);

    /* Record this key; if the set is full, evict the oldest entry, then rewrite
     * the whole file (updating mtime, which re-arms the rate floor). */
    if (nset >= BRIEF_SET_MAX) {
        int oldest = 0;
        for (int i = 1; i < nset; i++) if (tss[i] < tss[oldest]) oldest = i;
        for (int i = oldest; i + 1 < nset; i++) {
            memcpy(keys[i], keys[i + 1], sizeof keys[i]);
            tss[i] = tss[i + 1];
        }
        nset--;
    }
    snprintf(keys[nset], sizeof keys[nset], "%s", key);
    tss[nset] = now;
    nset++;

    f = fopen(path, "w");
    if (f) {
        for (int i = 0; i < nset; i++)
            fprintf(f, "%s\t%ld\n", keys[i], tss[i]);
        fclose(f);
    }
}

/* ══ JINV-068 — UM leitor de programa. Arquivo e stdin usam este. ═════════
 *
 * Lê um fd até EOF crescendo o buffer (dobra), devolve NUL-terminado, e nunca
 * entrega um programa PELA METADE: falha de alocação devolve NULL e o chamador
 * erra alto. É a mesma disciplina do JINV-007 (script truncado executado com
 * rc 0), agora em UMA implementação em vez de uma por porta de entrada.
 *
 * Por que existe: o leitor de stdin era orientado a LINHA — avaliava cada
 * linha assim que a lia — enquanto o de arquivo acumulava o programa inteiro
 * e o entregava a jash_run_multiline. Efeito: toda construção multi-linha
 * (if/for/while/case/função/heredoc) funcionava via `jash script.sh` e
 * QUEBRAVA via `jash < script.sh` ou `curl … | jash`, que é como metade do
 * mundo instala software. Ver JINV-068 abaixo, no chamador. */
static char* jash_slurp_program(long fd, size_t* out_len) {
    size_t cap = 65536;
    char* buf = (char*)malloc(cap);
    if (!buf) return NULL;
    size_t len = 0;
    for (;;) {
        if (len + 1 >= cap) {                     /* precisa crescer */
            size_t ncap = cap * 2;
            char* nb = (char*)realloc(buf, ncap);
            if (!nb) { free(buf); return NULL; }  /* falha ALTA, nunca parcial */
            buf = nb;
            cap = ncap;
        }
        long n = sys_read(fd, buf + len, cap - len - 1);
        if (n <= 0) break;
        len += (size_t)n;
    }
    buf[len] = '\0';
    if (out_len) *out_len = len;
    return buf;
}

static int jash_main_state_return(jash_state_t* state, int status) {
    jash_state_cleanup(state);
    hd_cleanup_temps();
    free(state);
    return status;
}

/* Optional result channel, pinned before --exec-json applies request env.
 * Neither command stderr nor an exec'd child inherits the retained descriptor. */
static int agent_result_fd_init(void) {
    const char* value = getenv("JASH_AGENT_FD");
    if (!value) return -1;
    if (!*value) { errno = EINVAL; return -2; }
    for (const char* p = value; *p; p++)
        if (*p < '0' || *p > '9') { errno = EINVAL; return -2; }
    char* end;
    errno = 0;
    long source = strtol(value, &end, 10);
    if (errno || *end || source < 3 || source > INT_MAX) { errno = EINVAL; return -2; }
    int flags = fcntl((int)source, F_GETFL);
    if (flags < 0) return -2;
    if ((flags & O_ACCMODE) == O_RDONLY) { errno = EBADF; return -2; }
#ifdef F_DUPFD_CLOEXEC
    int fd = fcntl((int)source, F_DUPFD_CLOEXEC, 64);
#else
    int fd = fcntl((int)source, F_DUPFD, 64);
    if (fd >= 0 && fcntl(fd, F_SETFD, FD_CLOEXEC) != 0) {
        int error = errno; close(fd); errno = error; return -2;
    }
#endif
    if (fd < 0) return -2;
    if (close((int)source) != 0 || unsetenv("JASH_AGENT_FD") != 0 || setenv("JASH_AGENT", "1", 1) != 0) {
        int error = errno; close(fd); errno = error; return -2;
    }
    return fd;
}

int main(int argc, const char* argv[]) {
    /* Loader policy is startup state, not mutable shell state. Pin HOME, key,
     * strict knobs, and policy bytes before any command or jashrc can alter it. */
    jash_module_security_init();
    /* --version / -V: identify and exit. A shell that "thinks for the LLM"
     * must answer when asked who it is. Checked on argv[1] only, so it never
     * shadows `jash -c '--version'` (there --version is the command text). */
    if (argc >= 2 && argv[1] &&
        (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)) {
        printf("jash %s (%s)\n", JASH_VERSION, JASH_CODENAME);
        return 0;
    }
    /* --whoami: machine-readable build id for drift detection (Wave 7). An
     * agent that builds + installs jash can compare this to the binary it just
     * produced and catch "I'm talking to a stale me" before wasting a turn. */
#ifndef JASH_GITHASH
#define JASH_GITHASH "unknown"
#endif
    if (argc >= 2 && argv[1] && strcmp(argv[1], "--whoami") == 0) {
        printf("jash version=%s codename=%s git=%s modabi=%d\n",
               JASH_VERSION, JASH_CODENAME, JASH_GITHASH, JMOD_ABI_VERSION);
        return 0;
    }
    if (argc >= 2 && argv[1] && strcmp(argv[1], "--capabilities-json") == 0) {
        return jash_capabilities_json();   /* JINV-018 */
    }
    if (argc >= 2 && argv[1] && strcmp(argv[1], "--builtins-json") == 0) {
        return jash_builtins_json();       /* JINV-047 */
    }
    if (argc >= 2 && !strcmp(argv[1], "--contract-json")) {
        if (argc > 3) {
            fputs("jash: --contract-json accepts at most one operation id\n", stderr);
            return 2;
        }
        int fd = STDOUT_FILENO;
        return jash_agent_contract_json(argc == 3 ? argv[2] : NULL,
                                        jash_jsonw_fd_sink, &fd);
    }
    /* --agent-guide: emit the auto-generated field guide for agents. This is
     * the antidote to "agent does not know jash primitives". Print it, save
     * it, paste it into the system prompt — but make it discoverable. */
    if (argc >= 2 && argv[1] && strcmp(argv[1], "--agent-guide") == 0) {
        return jash_b_agent_guide(NULL, jash_stdout_writer, 0, "--markdown");
    }
    int agent_result_fd = agent_result_fd_init();
    if (agent_result_fd == -2) {
        fprintf(stderr, "jash: invalid JASH_AGENT_FD: %s\n", strerror(errno));
        return 125;
    }
    /* --serve: structured exec protocol daemon (ADR-0038 W6) — one persistent
     * attested jash speaking JSONL on stdio. Detected here on argv[1]; the
     * actual loop starts after full state init below. */
    int serve_mode = (argc >= 2 && argv[1] && strcmp(argv[1], "--serve") == 0);

    /* Match bash collation for glob expansion, [[ ]] ordering, etc. */
    setlocale(LC_COLLATE, "");

    /* Beyond: -i flag forces interactive mode (needed when running as /init) */
    int force_interactive = 0;
    if (argc >= 2 && argv[1] && strcmp(argv[1], "-i") == 0) {
        force_interactive = 1;
    }

    /* F9: self-healing temp. If $TMPDIR (or /tmp) is missing/unwritable, fall
     * back to ~/.jash/tmp so jash's own temp ops (timeline backups, output
     * overflow, module extraction) never fail. One cheap access() check; the
     * full-disk/quota case is surfaced by `doctor`. */
    {
        const char* td = getenv("TMPDIR");
        const char* probe = (td && td[0]) ? td : jash_tmpdir();
        if (access(probe, W_OK) != 0) {
            const char* home = getenv("HOME");
            if (home && home[0]) {
                char jd[400];
                snprintf(jd, sizeof(jd), "%s/.jash", home);     mkdir(jd, 0700);
                snprintf(jd, sizeof(jd), "%s/.jash/tmp", home); mkdir(jd, 0700);
                setenv("TMPDIR", jd, 1);
            }
        }
    }

    /* ADR-0034: Binary integrity auto-check. Run on every startup to detect
     * if the jash binary has been replaced since last store. Alert the user
     * but don't block execution — this is a warning, not a gate.
     * 
     * JINV-103-STARTUP: Skip heavy selfcheck in -c mode for 20ms speedup.
     * Set JASH_SELFCHECK=1 to force check. Selfcheck runs lazily on first
     * use or when explicitly requested. */

    /* Parse command-line flags FIRST so we can decide on banner BEFORE
     * doing any heap allocation. This lets the banner hit the wire with
     * essentially zero in-process work — the user sees jash before any
     * state is allocated. F E R M I ∞ H A R T */
    int is_command_mode = 0;
    int is_script_mode = 0;
    const char* command_str = 0;
    const char* surface_tag = 0;
    const char* script_path = 0;
    int script_argi = -1;  /* BUG-112: argv index of the script path */
    int command_argi = -1; /* argv after the -c string: $0, then $1..$N */

    /* ══ JINV-067 — flag antes do `-c` NÃO PODE engolir o comando ══════════
     *
     * O laço abaixo conhecia exatamente quatro letras (`l i s c`). Qualquer
     * outra caía num `break` calado, e como o argumento restante começava com
     * '-' também não virava script: o processo ia parar na REPL, lia um stdin
     * vazio e saía 0. Medido:
     *
     *   $ jash -e -c 'echo OK' ; echo $?      (nada)  0
     *   $ jash -x -c 'echo OK' ; echo $?      (nada)  0
     *   $ jash -ec 'echo OK'   ; echo $?      (nada)  0
     *   $ jash -o errexit -c 'echo OK'        (nada)  0
     *   $ jash -e script.sh    ; echo $?      (nada)  0   ← script também
     *
     * `sh -e -c …` / `sh -eu -c …` é a forma canônica de invocar shell em CI,
     * Makefile, Dockerfile e unidade systemd. Um pipeline inteiro passava
     * VERDE sem ter executado UMA linha. É a mesma raiz do JINV-045 (que matou
     * isto para flags LONGAS: `--xxx` devolve rc 2 nomeando a flag) e do
     * JINV-017/044: aceitar-e-ignorar. Aqui era pior que ignorar — levava
     * junto o argumento seguinte.
     *
     * Conserto, em duas partes:
     *
     *  (a) as letras reais do POSIX/bash (-e -u -x -v -f -a -b -C -m -h -k -p
     *      -t e -o NOME) são ACEITAS e APLICADAS, pela MESMA porta do builtin
     *      `set` (jash_shell_opt_short/long, c/builtins/builtin_set.c). Uma
     *      tabela só: o que `set -e` faz é por construção o que `-e` faz.
     *      Validação acontece AQUI (com s == NULL, sem escrever estado), e a
     *      aplicação logo depois de jash_state_init — o estado ainda nem
     *      existe neste ponto do arranque.
     *
     *  (b) letra genuinamente desconhecida é RECUSADA com rc 2 nomeando-a.
     *      Nunca engolida.
     *
     * `+e` (a forma de desligar do bash) entra pelo mesmo caminho — daí o '+'
     * na condição do laço.
     *
     * Gate: tests/test_jinv067_invocation_flags.sh
     *
     * Formas combinadas continuam valendo: `-ec 'cmd'`, `-lc 'cmd'`,
     * `-li -c 'cmd'`. --long-flags (--agent etc.) seguem tratadas abaixo. */
    #define JASH_PEND_OPT_MAX 64
    struct { char letra; const char* nome; int liga; } pend_opt[JASH_PEND_OPT_MAX];
    int pend_opt_n = 0;

    int argi = 1;
    int agent_mode = 0;
    int bg_mode = 0;
    while (argi < argc && argv[argi] &&
           (argv[argi][0] == '-' || argv[argi][0] == '+')) {
        const char* a = argv[argi];
        if (strcmp(a, "--surface") == 0) {
            if (surface_tag || argi + 1 >= argc || !argv[argi + 1]) {
                fputs("jash: --surface requires exactly one tag\n", stderr);
                return 2;
            }
            surface_tag = argv[argi + 1];
            argi += 2;
            continue;
        }
        /* --exec-json '{"argv":[...],"cwd":…,"env":{…},"stdin":"…"}':
         * entrada estruturada e literal. JINV-017: uma porta só
         * (core/serve.c), o MESMO protocolo do `--serve` — com cwd/env/stdin
         * de fato aplicados e um erro legível quando o pedido não pode ser
         * honrado (fail-closed: erro aqui significa que NADA rodou). */
        if (strcmp(a, "--exec-json") == 0) {
            if (argi + 1 >= argc) {
                static const char m[] = "jash: --exec-json: requires a JSON argument\n";
                (void)!write(2, m, sizeof(m) - 1); return 2;
            }
            static char ej_cmd[JASH_EXEC_JSON_LINE_MAX];
            const char* ej_err = jash_exec_json(argv[argi + 1], ej_cmd,
                                                sizeof ej_cmd, 1);
            if (ej_err) {
                fprintf(stderr, "jash: --exec-json: %s\n", ej_err);
                fputs("jash: campos aceitos: argv[] | line, cwd, env{}, stdin\n",
                      stderr);
                return 2;
            }
            is_command_mode = 1; command_str = ej_cmd; argi += 2; break;
        }
        /* JINV-045 — flag longa desconhecida FALHA ALTO.
         *
         * Este ramo pulava QUALQUER `--algo` em silencio. Consequencia medida:
         *
         *   $ jash --flag-que-nao-existe ; echo $?
         *   (saida vazia)  0
         *
         * Saida vazia com rc=0 e indistinguivel de "a flag existe e nao
         * imprimiu nada". Quem sonda o binario para descobrir se uma feature
         * chegou — que e exatamente o que um agente faz — nao consegue
         * diferenciar "nao existe" de "existe e esta vazio". Custou tempo real
         * a quem reportou: so descobriu com um teste de controle usando uma
         * flag inventada.
         *
         * Mesma raiz do JINV-017 e do JINV-044: aceitar-e-ignorar. Um erro
         * alto custa um segundo.
         *
         * A lista abaixo e a das flags longas realmente tratadas em main().
         * `--` sozinho e o fim-de-opcoes do POSIX e segue passando. */
        if (a[0] == '-' && a[1] == '-') {
            /* `--` sozinho é o fim-de-opções do POSIX: o que vem depois é
             * script/argumento, nunca flag. Antes isto era `continue`, então
             * `jash -- -arquivo.sh` voltava a interpretar `-arquivo.sh` como
             * cluster de flags. */
            if (a[2] == '\0') { argi++; break; }
            static const char* const conhecidas[] = {
                "--", "--version", "--whoami", "--agent-guide", "--agent",
                "--capabilities-json", "--builtins-json", "--exec-json",
                "--serve", "--bg", "--surface",
                "--markdown", "--restore", "--login", "--interactive", 0
            };
            int ok = 0;
            for (int k = 0; conhecidas[k]; k++)
                if (strcmp(a, conhecidas[k]) == 0) { ok = 1; break; }
            if (!ok) {
                fprintf(stderr,
                        "jash: opcao desconhecida '%s'\n"
                        "jash: veja `jash --agent-guide` para as flags "
                        "aceitas, ou `jash --capabilities-json` para o que "
                        "este build de fato faz\n", a);
                return 2;
            }
            if (strcmp(a, "--agent") == 0) agent_mode = 1;
            if (strcmp(a, "--bg") == 0) bg_mode = 1;
            argi++; continue;
        }
        if (a[1] == '\0') break;  /* '-' ou '+' sozinho */

        /* '-x' liga, '+x' desliga — a convenção do POSIX/bash para opções. */
        int liga = (a[0] == '-');
        int has_c = 0;
        int extra = 0;   /* argv consumidos ALÉM deste cluster (operando de -o) */

        for (size_t k = 1; a[k]; k++) {
            char f = a[k];
            /* Letras de INVOCAÇÃO (não são opções de `set`): só o jash as vê. */
            if (f == 'c') { has_c = 1; continue; }
            if (f == 'l' || f == 'i' || f == 's') continue;

            if (f == 'o') {
                /* `-o NOME`: o nome é o resto do cluster (`-oerrexit`) ou o
                 * argv seguinte (`-o errexit`). */
                const char* nome = a + k + 1;
                if (!*nome) {
                    if (argi + 1 < argc && argv[argi + 1]) {
                        nome = argv[argi + 1];
                        extra = 1;
                    } else {
                        nome = "";
                    }
                }
                int r = jash_shell_opt_long(NULL, nome, liga);
                if (r == JASH_OPT_UNSUPPORTED) {          /* JINV-069 */
                    char como[96];
                    snprintf(como, sizeof como, "-o %s", nome);
                    jash_shell_opt_noexec_refuse(como);
                    return 2;
                }
                if (r != JASH_OPT_OK) {
                    fprintf(stderr,
                            "jash: -o: opcao de shell desconhecida '%s'\n"
                            "jash: aceitas: errexit nounset xtrace verbose "
                            "noglob noclobber monitor allexport notify hashall "
                            "keyword privileged onecmd pipefail posix emacs vi "
                            "errtrace\n", nome);
                    return 2;
                }
                /* JINV-029: nunca truncar em silêncio. Uma opção pedida e não
                 * registrada é uma opção aceita e ignorada — o defeito desta
                 * própria onda, reintroduzido pela porta de trás. */
                if (pend_opt_n >= JASH_PEND_OPT_MAX) {
                    fprintf(stderr, "jash: opcoes de invocacao demais "
                                    "(teto %d)\n", JASH_PEND_OPT_MAX);
                    return 2;
                }
                pend_opt[pend_opt_n].letra = 0;
                pend_opt[pend_opt_n].nome  = nome;
                pend_opt[pend_opt_n].liga  = liga;
                pend_opt_n++;
                break;   /* -o consome o resto do cluster */
            }

            /* Todas as demais letras passam pela tabela ÚNICA de `set`
             * (s == NULL: só valida, não escreve estado — ele nem existe
             * ainda). Aplicação real logo após jash_state_init. */
            int r = jash_shell_opt_short(NULL, f, liga);
            if (r == JASH_OPT_UNSUPPORTED) {              /* JINV-069 */
                char como[8];
                snprintf(como, sizeof como, "-%c", f);
                jash_shell_opt_noexec_refuse(como);
                return 2;
            }
            if (r != JASH_OPT_OK) {
                /* JINV-067: nomeia a flag. Sumir em silêncio é o defeito. */
                fprintf(stderr,
                        "jash: -%c: opcao invalida\n"
                        "jash: opcoes curtas aceitas: -c -i -l -s -e -u -x -v "
                        "-f -a -b -C -m -h -k -p -t -o NOME\n"
                        "jash: veja `jash --agent-guide` para as flags "
                        "aceitas, ou `jash --capabilities-json` para o que "
                        "este build de fato faz\n", f);
                return 2;
            }
            if (pend_opt_n >= JASH_PEND_OPT_MAX) {      /* JINV-029 */
                fprintf(stderr, "jash: opcoes de invocacao demais "
                                "(teto %d)\n", JASH_PEND_OPT_MAX);
                return 2;
            }
            pend_opt[pend_opt_n].letra = f;
            pend_opt[pend_opt_n].nome  = 0;
            pend_opt[pend_opt_n].liga  = liga;
            pend_opt_n++;
        }

        if (has_c) {
            is_command_mode = 1;
            if (argi + 1 + extra < argc) command_str = argv[argi + 1 + extra];
            else {
                static const char msg[] =
                    "jash: -c: option requires an argument\n";
                (void)!write(2, msg, sizeof(msg) - 1);
                return 2;
            }
            argi += extra + 2;   /* o cluster, o operando de -o, e a string */
            command_argi = argi;
            break;
        }
        argi += extra + 1;
    }

    /* Refuse before the literal fast path (or --bg) can run any command. */
    if (command_argi >= 0 && argc - command_argi - 1 > JASH_POS_MAX) {
        fprintf(stderr, "jash: -c: %d positional parameters exceed limit %d -- refusing\n",
                argc - command_argi - 1, JASH_POS_MAX);
        return 2;
    }

    if (agent_result_fd >= 0 && (!is_command_mode || bg_mode)) {
        close(agent_result_fd);
        fputs("jash: JASH_AGENT_FD requires foreground -c or --exec-json\n", stderr);
        return 125;
    }

    const char* startup_safe_env = getenv("JASH_SAFE");
    if (startup_safe_env && startup_safe_env[0] == '1') agent_mode = 1;

    if (!is_command_mode && argi < argc) {
        const char* arg = argv[argi];
        /* --restore flag from live-reload (processed later). */
        if (arg && arg[0] == '-' && arg[1] == '-' && arg[2] == 'r') {
            /* fall through */
        }
        /* Script file execution */
        else if (arg[0] != '-') {
            is_script_mode = 1;
            script_path = arg;
            script_argi = argi;  /* BUG-112: argv after this are $1, $2, … */
        }
    }

    if (surface_tag) {
        if (is_command_mode || is_script_mode || argi < argc) {
            fputs("jash: --surface cannot be combined with a command or script\n", stderr);
            return 2;
        }
#ifdef JASH_TARGET_MINIX
        int surface_rc = jash_minix_surface_open(surface_tag, &jash_minix_surface);
        if (surface_rc < 0) {
            fprintf(stderr, "jash: --surface: attach failed (%d)\n", surface_rc);
            return 2;
        }
        jash_surface_active = &jash_minix_surface;
#else
        fputs("jash: --surface is available only in MINIX builds\n", stderr);
        return 2;
#endif
    }
    
    /* ── ADR-0042 F6: `--bg` → detached async job ─────────────────────────────
     * Fork HERE, before the banner + 355 KB state init, so the parent returns
     * in well under 300 ms. Parent: allocate an id, record it running, print
     * {"job":N}, exit 0. Child: setsid (reparent to init → no zombie, no
     * reaper), redirect its combined stdout+stderr to ~/.jash/jobs/<id>.out,
     * mark JASH_BG_ID, and fall through into the normal init + -c path; the emit
     * block finalizes <id>.json on exit. A later -c learns of completion via the
     * envelope's one-shot "jobs_done". F E R M I ∞ H A R T */
    {
        if (bg_mode && is_command_mode && command_str) {
            int jid = jash_jobs_bg_alloc();
            if (jid > 0) {
                pid_t kid = fork();
                if (kid > 0) {
                    jash_jobs_bg_start(jid, (long)kid, command_str);
                    char line[64];
                    int ln = snprintf(line, sizeof line, "{\"job\":%d}\n", jid);
                    if (ln > 0) { ssize_t wr = write(1, line, (size_t)ln); (void)wr; }
                    _exit(0);
                } else if (kid == 0) {
                    setsid();
                    /* detached: no controlling tty on stdin */
                    int nfd = open("/dev/null", O_RDONLY);
                    if (nfd >= 0) { dup2(nfd, 0); if (nfd > 0) close(nfd); }
                    char op[680];
                    if (jash_jobs_bg_out_path(jid, op, sizeof op)) {
                        int ofd = open(op, O_WRONLY | O_CREAT | O_TRUNC, 0600);
                        if (ofd >= 0) {
                            dup2(ofd, 1); dup2(ofd, 2);
                            if (ofd > 2) close(ofd);
                        }
                    }
                    char idbuf[16];
                    snprintf(idbuf, sizeof idbuf, "%d", jid);
                    setenv("JASH_BG_ID", idbuf, 1);
                    /* fall through into the normal init + -c path */
                }
                /* kid < 0 (fork failed): run in the foreground as a fallback */
            }
        }
    }

    /* Critical init: selfcheck and token tracking must be available after
     * fork-bg child lands here, or for any -c/interactive path. */
    jash_selfcheck_auto();
    jash_token_init();

    /* Show banner BEFORE the 355KB state malloc. Banner is ~600 bytes of
     * ANSI to stdout — getting it out first means time-to-first-byte is
     * essentially exec + dyld + 1 syscall, while the state allocation and
     * full init run AFTER the user already sees the shell.
     *
     * Rule: a banner is UX; if either fd 0 or fd 1 is not a tty, we're
     * being piped/redirected and the user neither asked for nor wants
     * decoration.
     *
     * ALSO: if /etc/motd exists with non-whitespace content, login(1)/sshd
     * already showed it — duplicating with our banner is noise. Prefer the
     * user's /etc/motd. To force the jash banner regardless, set
     * JASH_BANNER=1 in the environment. F E R M I ∞ H A R T */
    if (!is_command_mode && !is_script_mode &&
        isatty(0) && isatty(1)) {
        /* Greeting order is jash's responsibility. macOS login(1) prints
         * /etc/motd ITSELF before the shell starts, leaving the cursor
         * below it — but jash's later prompt/statusbar setup could then
         * land above it, so the motd appeared *below* the prompt. To make
         * the layout deterministic (motd on top, prompt below) regardless
         * of how jash was launched (login, `exec jash`, new tab), JASH
         * prints the motd here itself. The companion `~/.hushlogin` (we
         * create it on first run) stops login from double-printing.
         *
         * Precedence: JASH_BANNER=1 forces the jash logo even with a motd;
         * otherwise a non-empty /etc/motd wins and the jash banner is
         * suppressed. F E R M I ∞ H A R T */
        const char* force = getenv("JASH_BANNER");
        int motd_shown = 0;

        if (!force || force[0] != '1') {
            long mfd = sys_open("/etc/motd", 0, 0);
            if (mfd >= 0) {
                char mbuf[4096];
                long mn = sys_read((int)mfd, mbuf, sizeof(mbuf) - 1);
                sys_close((int)mfd);
                if (mn > 0) {
                    mbuf[mn] = '\0';
                    int has_content = 0;
                    for (long k = 0; k < mn; k++) {
                        char c = mbuf[k];
                        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
                            has_content = 1;
                            break;
                        }
                    }
                    if (has_content) {
                        WRITE_IGNORE(STDOUT_FILENO, mbuf, (size_t)mn);
                        /* Ensure a trailing newline so the prompt starts
                         * on its own line below the motd. */
                        if (mbuf[mn - 1] != '\n')
                            WRITE_IGNORE(STDOUT_FILENO, "\n", 1);
                        motd_shown = 1;
                    }
                }
            }
        }

        if (!motd_shown) {
            jash_banner_render(jash_stdout_writer, 0, sys_getpid());
        }

        /* Create ~/.hushlogin once so macOS login stops printing the motd
         * (and the "Last login:" line) on top of ours. Best-effort; ignore
         * errors (read-only home, sandbox, etc.). */
        const char* home = getenv("HOME");
        if (home && *home) {
            char hush[1024];
            int hl = snprintf(hush, sizeof(hush), "%s/.hushlogin", home);
            if (hl > 0 && hl < (int)sizeof(hush)) {
                long hfd = sys_open(hush, 0, 0);
                if (hfd < 0) {
                    /* Not present — create it (O_CREAT|O_WRONLY = 0x201 on
                     * macOS; use the libc open to stay portable). */
                    extern int open(const char*, int, ...);
                    int cfd = open(hush, 0x0001 /*O_WRONLY*/ | 0x0200 /*O_CREAT*/, 0644);
                    if (cfd >= 0) close(cfd);
                } else {
                    sys_close((int)hfd);
                }
            }
        }
    }

    /* Load ~/.jashrc with agent defaults (JASH_GUARD, etc.) BEFORE ultra-fast path.
     * This ensures agent protections are enabled even for trivial -c commands. */
#ifndef JASH_TINALINUX
    {
        const char* home = getenv("HOME");
        if (home && *home) {
            char jashrc_path[512];
            snprintf(jashrc_path, sizeof(jashrc_path), "%s/.jashrc", home);
            FILE* f = fopen(jashrc_path, "r");

            /* The usual read needs no existence probe; retain first-run setup
             * only on the failed-open path, without overwriting unreadable files. */
            if (!f && access(jashrc_path, F_OK) != 0) {
                f = fopen(jashrc_path, "w");
                if (f) {
                    fputs("# ~/.jashrc — jash configuration file (auto-created on first run)\n", f);
                    fputs("# Agent defaults - enables anti-bypass monitoring by default\n", f);
                    fputs("export JASH_GUARD=1\n", f);
                    fputs("export JASH_GUARD_BYPASS=1\n", f);
                    fputs("export JASH_GUARD_BYPASS_STRICT=1\n", f);
                    fclose(f);
                    f = NULL;
                }
            }

            if (!f) f = fopen(jashrc_path, "r");
            if (f) {
                char line[1024];
                while (fgets(line, sizeof(line), f)) {
                    char* eq = strchr(line, '=');
                    if (eq && strncmp(line, "export", 6) == 0) {
                        *eq = '\0';
                        char* key = line + 6;
                        while (*key == ' ' || *key == '\t') key++;
                        char* val = eq + 1;
                        while (*val == ' ' || *val == '\t') val++;
                        char* nl = strchr(val, '\n');
                        if (nl) *nl = '\0';
                        /* ADR-0042 F7: env explícito vence .jashrc.
                         * setenv(..., 1) fazia JASH_GUARD_BYPASS_STRICT=1 do
                         * .jashrc silenciosamente anular qualquer prefixo da
                         * linha de comando — inclusive testes que desligam o
                         * guard. 0 significa "não sobrescreva se já veio do
                         * ambiente do pai". */
                        setenv(key, val, 0);
                    }
                }
                fclose(f);
            }
        }
    }
#endif

    const char* configured_safe_env = getenv("JASH_SAFE");
    if (configured_safe_env && configured_safe_env[0] == '1') agent_mode = 1;

    /* ULTRA-FAST `-c` path: handle trivial commands BEFORE state malloc
     * so `jash -c exit`, `jash -c 'echo hi'`, `jash -c 'rm foo'` pay
     * zero shell-init cost. Falls through to normal init for anything else.
     * This is safe AFTER flag parsing (is_command_mode is known) but BEFORE
     * allocating ~350KB jash_state_t. */
    if (is_command_mode && argc >= 3 && argv[1] && argv[1][0] == '-' && argv[1][1] == 'c' && argv[1][2] == '\0') {
        const char* _sig_env = getenv("JASH_SIGIL");
        int _sig_explicit_off = _sig_env && atoi(_sig_env) == 0;
        const char* _brain_env = getenv("JASH_BRAIN");
        int _brain_bypass_fast = _brain_env && (_brain_env[0] == '1' || _brain_env[0] == 't' || _brain_env[0] == 'y');
        if (!agent_mode && !jash_agent_enabled() && _sig_explicit_off &&
            !_brain_bypass_fast) {
            int fast_rc = 0;
            int allow_redirect = pend_opt_n == 0 &&
                !getenv("JASH_SESSION") && !jash_env_smells_like_agent();
            if (ultra_fast_c_path(argv[2], &fast_rc, allow_redirect)) {
                jash_agent_first_contact_brief();
                return fast_rc;
            }
        }
    }

    /* jash_state_t is ~350 KB (env table + history ring + aliases +
     * yank). The user stack on Josh is currently 64 pages = 256 KB,
     * which cannot hold it.
     *
     * Progressive boot: use malloc to keep jash_state_t out of BSS.
     * This reduces the DATA segment from 659KB to ~300KB, cutting
     * page faults on startup. The struct is heap-allocated and released
     * through jash_main_state_return() on every post-init exit. */
    jash_state_t* state_p = (jash_state_t*)malloc(sizeof(jash_state_t));
    if (!state_p) {
        /* Fail loudly here — every line below dereferences `state`
         * unconditionally, so a silent NULL would surface as an obscure
         * segfault deep in some unrelated function instead of a clear
         * "out of memory" at the one place that actually knows why. */
        const char* msg = "jash: out of memory during startup\n";
        sys_write(2, msg, strlen(msg));
        sys_exit(1);
    }
    jash_state_t* state = state_p;  /* convenience alias */
    double _t0 = _bench_now();
    state->pid = sys_getpid();
#ifdef JASH_TINALINUX
    /* The Surface device, unlike a TTY, has its own input policy and seat. */
    (void)jash_tina_surface_probe();
    if (!is_command_mode && !is_script_mode && state->pid != 1) {
        const char* lin = getenv("TINA_LINEAGE");
        int valid = lin != NULL;
        if (valid) {
            for (int i = 0; i < 64; i++) {
                char c = lin[i];
                if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                      (c >= 'A' && c <= 'F'))) { valid = 0; break; }
            }
            if (valid && lin[64] != 0) valid = 0;
        }
        if (!valid) {
            fputs("jash: no birth proof (TINA_LINEAGE absent or malformed) — refusing interactive shell\n", stderr);
            return 3;
        }
    }
#endif
    
    /* ── CRITICAL INIT ONLY (must be fast for progressive boot) ───── */
    jash_sigil_auto_init();  /* honor JASH_SIGIL env var (A1: Claude Code) */
    /* Journal, context engine, and stances are only needed in interactive/REPL
     * sessions. Skip for -c and script mode to shave startup latency. */
    if (!is_command_mode && !is_script_mode) {
        journal_init();
        context_init();
        stance_init();
    }
    /* Recording-on by default for NON-INTERACTIVE (agent/script/pipe) sessions when
     * the user hasn't chosen a level: memory (recall), undo (rewind/undo), the auto
     * safety floor, and adoption metrics then accrue AUTOMATICALLY — a bash-habit
     * agent is already better off than on bash, with zero behavior change. Explicit
     * JASH_SIGIL always wins; interactive humans stay opt-in (no surprise logging). */
    if (!getenv("JASH_SIGIL")
#ifdef JASH_TARGET_MINIX
        && !jash_surface_active
#endif
        &&
        (is_command_mode || is_script_mode ||
#ifdef JASH_TINALINUX
         !input_is_interactive() || !output_is_interactive()))
#else
         !isatty(0) || !isatty(1)))
#endif
        jash_sigil_set(SIGIL_LOG);
    
    if (jash_sigil_get() >= SIGIL_LOG)
        jash_timeline_restore();
    jash_state_init(state);

    /* JINV-103: durable memory (brain) initializes lazily on first use
     * (see jash_brain_ensure): eager init here cost a 1024-fact memset +
     * stat/mkdir + full file load on every `-c`, including runs that never
     * consult the brain. The `brain` builtin and the liquid hook both enter
     * through the public API, which ensures init with identical state. */

    /* JINV-067 — aplica as opções colhidas do argv, na ORDEM em que vieram,
     * pela MESMA porta do builtin `set`. Elas já foram validadas lá em cima
     * (modo s == NULL); aqui só resta escrever o estado, que só agora existe.
     * `-e` antes de `-c` tem que valer para o comando inteiro, então isto
     * precisa acontecer ANTES de qualquer avaliação. */
    for (int pi = 0; pi < pend_opt_n; pi++) {
        int r = pend_opt[pi].nome
              ? jash_shell_opt_long(state, pend_opt[pi].nome, pend_opt[pi].liga)
              : jash_shell_opt_short(state, pend_opt[pi].letra, pend_opt[pi].liga);
        if (r != JASH_OPT_OK) {
            /* Inalcançável: a validação acima já recusaria. Se acontecer, as
             * duas passagens divergiram — falha ALTO em vez de rodar com uma
             * opção que o usuário pediu e não foi aplicada. */
            fprintf(stderr, "jash: opcao '%s%c' validada e nao aplicada "
                            "(bug interno)\n",
                    pend_opt[pi].nome ? pend_opt[pi].nome : "-",
                    pend_opt[pi].nome ? ' ' : pend_opt[pi].letra);
            return jash_main_state_return(state, 2);
        }
    }

    /* ── Context Engine: register built-in providers (needs state) ── */
    if (!is_command_mode && !is_script_mode)
        context_register_builtins((void*)state);

    /* ── Wave 10: Detect Beyond surface and enable native cell API ── */
    /* TODO: jash_surface_detect() hangs — ioctl may be blocking. Disable for now. */
    /* jash_surface_detect(); */

    /* ── Agent bootstrap: JASH_SAFE=1 or --agent loads safety stack ── */
    {
        const char* safe_env = getenv("JASH_SAFE");
        if (safe_env && safe_env[0] == '1') agent_mode = 1;
        int required_sigil_level = jash_sigil_get();
        if (agent_mode && required_sigil_level < SIGIL_SIGN)
            required_sigil_level = SIGIL_SIGN;
        jash_sigil_set(required_sigil_level);
        /* ROADMAP W1.1 / gap #2: the BLAKE3 attestation chain + proof live in
         * sigil.modjash (its post_cmd hook writes ~/.jash/chain.log). The core
         * jash_sigil_post_cmd only writes a plain commands.log. Load the module
         * whenever sigil logging is enabled (JASH_SIGIL>=1) — not only in agent
         * mode — so `JASH_SIGIL=2` actually produces the cryptographic chain the
         * integration guide (CLAUDE.md) promises. */
#ifndef JASH_TINALINUX
        if ((agent_mode || jash_sigil_get() >= SIGIL_LOG) && !jmod_find("sigil")) {
            char spath[PATH_MAX];
            int sigil_loaded = jmod_resolve_installed("sigil", spath, sizeof(spath)) &&
                               jmod_load_expected(spath, 0xFFFFFFFF, "sigil") == 0;
            if (!sigil_loaded && required_sigil_level >= SIGIL_LOG) {
                /* Honesty over silent degradation: if logging was requested but
                 * the module is absent, refuse to run unless the user explicitly
                 * opts into degraded mode. (P0-2) */
#ifdef JASH_BETHOS
                /* BethOS has no mutable HOME module store. Its attestation
                 * boundary is the signed image and typed surface ABI. */
                fprintf(stderr,
                    "jash: sigil: surface image has no mutable sigil module; "
                    "using signed-image attestation\n");
#else
                const char* allow_degraded = getenv("JASH_SIGIL_ALLOW_DEGRADED");
                if (allow_degraded && strcmp(allow_degraded, "1") == 0) {
                    fprintf(stderr,
                        "jash: sigil: JASH_SIGIL=%d set but sigil.modjash unavailable "
                        "(install to ~/.jash/modules/) — cryptographic chain disabled "
                        "(JASH_SIGIL_ALLOW_DEGRADED=1)\n",
                        jash_sigil_get());
                }
#ifdef JASH_TINALINUX
                else if (getenv("JASH_SIGIL")) {
                    fprintf(stderr,
                        "jash: sigil: JASH_SIGIL=%d set but sigil.modjash unavailable — cryptographic chain disabled\n",
                        jash_sigil_get());
                }
#else
                else {
                    fprintf(stderr,
                        "jash: sigil: JASH_SIGIL=%d set but sigil.modjash unavailable "
                        "— refusing to run without attestation module "
                        "(set JASH_SIGIL_ALLOW_DEGRADED=1 to override)\n",
                        jash_sigil_get());
                    return jash_main_state_return(state, 125);
                }
#endif
#endif
            }
        }
#endif
#ifdef JASH_TINALINUX
        /* Guest startup never autoloads mutable modules from $HOME. */
        if (agent_mode) {
            fputs("jash: agent mode requires trusted guest modules\n", stderr);
            return jash_main_state_return(state, 125);
        }
#else
        if (agent_mode) {
            /* Load undo.modjash */
            char upath[PATH_MAX];
            int undo_loaded =
                jmod_resolve_installed("undo", upath, sizeof(upath)) &&
                jmod_load_expected(upath, 0xFFFFFFFF, "undo") == 0;
            if (!undo_loaded) {
                fprintf(stderr,
                        "jash: safe mode requires trusted undo.modjash; refusing to run\n");
                return jash_main_state_return(state, 125);
            }
            /* tx safe on */
            jmod_call_t safe_call = { state, jash_stdout_writer, 0, NULL, "safe on" };
            if (jmod_dispatch("tx", &safe_call) != 0) {
                fprintf(stderr,
                        "jash: safe mode could not enable transactional safety; refusing to run\n");
                    return jash_main_state_return(state, 125);
                }
            }
#endif
    }

    /* Non-interactive when: -c mode, script mode, OR stdin/stdout not a tty.
     * The tty check kills statusbar/spinner/render_prompt noise when jash is
     * piped (echo cmd | jash) — they all read state->interactive.
     * Exception: -i flag forces interactive mode (Beyond /init). */
    if (!force_interactive &&
        (is_command_mode || is_script_mode ||
#ifdef JASH_TINALINUX
         !input_is_interactive() || !output_is_interactive()))
#else
        !isatty(0) || !isatty(1)))
#endif
        state->interactive = 0;
#ifdef JASH_TARGET_MINIX
    if (jash_surface_active) state->interactive = 0;
#endif

    /* If we're taking over an interactive terminal, make sure it is in a sane
     * canonical state. This fixes `exec jash` from a jash whose readline left
     * the tty in raw mode. */
    if (state->interactive) terminal_ensure_sane();

    /* ── SMART CACHE: Initialize command substitution cache ───────── */
    cache_init();
    jash_env_seed_defaults(state);
    if (!state->pid) state->pid = sys_getpid();
    double _t1 = _bench_now();

    /* ── Job control setup ────────────────────────────────────────────── */
    state->shell_tty_fd = STDIN_FILENO;
    state->shell_pgid = (int)getpid();
    state->job_control_active = 0;
#ifndef JASH_TARGET_BEYOND
    /* Beyond: no termios, no job control, no tcsetpgrp */
    if (
#ifdef JASH_TARGET_MINIX
        !jash_surface_active &&
#endif
        isatty(STDIN_FILENO)
#ifdef JASH_TINALINUX
        || jash_tina_surface_native_input()
#endif
    ) {
        pid_t mypgid = getpid();
        int inherited_surface_group = 0;
#ifdef JASH_TINALINUX
        /* Only the endpoint owner can publish a new foreground group. Nested
         * shells inherit the granted group rather than creating an unreadable
         * private group and ignoring the failed SURF_SET_FOREGROUND. */
        inherited_surface_group = jash_tina_surface_native_input() &&
                                  !jash_tina_surface_foreground_owner();
        if (inherited_surface_group) mypgid = getpgrp();
#endif
        if (inherited_surface_group || setpgid(mypgid, mypgid) == 0 || errno == EPERM) {
            state->shell_pgid = (int)mypgid;
            signal(SIGTTOU, SIG_IGN);
            signal(SIGTTIN, SIG_IGN);
            signal(SIGTSTP, SIG_IGN);
#ifdef JASH_TINALINUX
            signal(SIGINT, jash_sigint_forward);
#else
            signal(SIGINT, SIG_IGN);
#endif
            signal(SIGQUIT, SIG_IGN);
#ifdef JASH_TINALINUX
            if (jash_tina_surface_native_input()) {
                if (!inherited_surface_group)
                    (void)jash_tina_surface_foreground((int)mypgid);
            } else
#endif
            if (tcsetpgrp(STDIN_FILENO, mypgid) == 0)
                state->job_control_active = 1;
        }
    }
#endif

    /* ADR-0038 W6: serve mode — one persistent shell for the whole
     * conversation. Shares the -c critical init (history, F1 session), then
     * hands stdio to the JSONL protocol loop until EOF. */
    if (serve_mode) {
        history_init();
#ifndef JASH_TINALINUX
        jash_session_load(state);
#endif
        int srv_rc = jash_serve_loop(state);
#ifndef JASH_TINALINUX
        jash_session_save(state);
#endif
        return jash_main_state_return(state, srv_rc);
    }

    /* -c is the hot non-interactive path: no history thread, no statusbar,
     * no frecency, no predictions. Execute directly after critical init. */
    if (is_command_mode && command_str) {
        /* ADR-0037: brief the agent once per session, on stderr, BEFORE the
         * command output so the model reads its identity first. */
        jash_agent_first_contact_brief();
        /* Load persistent history so `fc` and history builtins work in -c
         * mode and scripts, not just interactive sessions. */
        history_init();
        /* F1: restore cwd+env+aliases for the agent session (JASH_SESSION),
         * so separate `jash -c` calls share one logical shell. No-op when
         * JASH_SESSION is unset. */
#ifndef JASH_TINALINUX
        jash_session_load(state);
#endif
        /* Explicit invocation arguments override session state. With no $0
         * operand, a persistent session keeps its existing positional frame. */
        const char* positional_session = getenv("JASH_SESSION");
        if (command_argi >= 0 && (command_argi < argc ||
            !positional_session || !*positional_session)) {
            int supplied = command_argi < argc ? argc - command_argi - 1 : 0;
            const char* zero = command_argi < argc ? argv[command_argi] : argv[0];
            if (jash_env_set(state, "0", zero) != 0 ||
                jash_env_replace_positionals(state,
                    supplied ? &argv[command_argi + 1] : NULL, supplied) != 0) {
                fputs("jash: -c: positional storage failed -- refusing\n", stderr);
                return jash_main_state_return(state, 2);
            }
        }
        /* ADR-0042 F2: world-delta is computed below with the fschange
         * pre-snapshot when possible, avoiding two adjacent tree walks. */
        jash_world_delta_t* g_world_delta = NULL;
        /* BUG-063 / ADR-0038 W5+W8: record this command into the per-session
         * log (impact map + handoff read it). Agent sessions only — a human's
         * every keystroke doesn't need a durable record here. */
        if (jash_env_smells_like_agent()) jash_record_init(state);
        /* Process backslash-newline continuation (POSIX: <backslash><newline>
         * is removed, joining lines). Without this, 'echo \<newline>hello'
         * outputs literal backslash instead of joining to 'echo hello'.
         * Dynamic alloc only when a join might occur -- otherwise use the
         * original command. The prior 8KB buffer silently truncated payloads. */
        char* cmd_processed = strstr(command_str, "\\\n")
            ? (char*)malloc(strlen(command_str) + 1) : NULL;
        if (cmd_processed) {
            const char* src = command_str;
            unsigned di = 0;
            /* BUG-114: the join must be context-aware — a backslash inside a
             * COMMENT (`# note \`) or single quotes must NOT join lines
             * (inside '…' bash keeps \<newline> literal; a comment ends at
             * the newline regardless). */
            int csq = 0, ccmt = 0; char cprev = '\n';
            while (*src) {
                char cc = *src;
                if (cc == '\n') ccmt = 0;
                if (!csq && !ccmt && cc == '\\' && src[1] == '\n') {
                    src += 2;
                    cprev = ' ';
                    continue;
                }
                if (!ccmt) {
                    if (cc == '\'') csq = !csq;
                    else if (!csq && cc == '#' &&
                             (cprev == ' ' || cprev == '\t' || cprev == '\n' ||
                              cprev == ';' || cprev == '(' || cprev == '|' ||
                              cprev == '&')) ccmt = 1;
                }
                cprev = cc;
                cmd_processed[di++] = *src++;
            }
            cmd_processed[di] = '\0';
            command_str = cmd_processed;
        }

        double _t2 = _bench_now();
        int code = 0;

        /* ADR-0034: Wrap writer to count output bytes for real token tracking. */
        jash_token_wrap_t token_wrap;
        jash_token_wrap_init(&token_wrap, jash_highlighted_stdout_writer, 0);
        jash_writer_t token_writer = jash_token_wrap_writer(&token_wrap);
        /* ADR-0038 W1: register the writer pairs that mean "the agent's own
         * stdout" — memoization refuses anything else (captures, pipes). The
         * single-line path evaluates through token_writer; run_multiline
         * evaluates through the highlighted writer directly. */
        {
            extern void jash_memo_mark_top(jash_writer_t w, void* ctx);
            jash_memo_mark_top(token_writer, &token_wrap);
            jash_memo_mark_top(jash_highlighted_stdout_writer, 0);
        }

        /* ADR-0038 W2: snapshot the cwd tree so the epilogue can report the
         * command's REAL filesystem blast radius (what externals did — the
         * timeline only sees jash's own journaled mutations). One snapshot
         * per -c invocation (tool-call granularity), agent sessions only,
         * skipped for obviously-pure commands. JASH_CHANGES=0 disables. */
        jash_fssnap_t* fs_snap = NULL;
        jash_world_scan_t* shared_world_scan = NULL;
        char fs_root[1024] = "";
        {
            const char* chg = getenv("JASH_CHANGES");
            if (!(chg && chg[0] == '0') && jash_env_smells_like_agent() &&
                !jash_fschange_command_is_pure(command_str)) {
                if (getcwd(fs_root, sizeof(fs_root))) {
                    const char* sid = getenv("JASH_SESSION");
                    const char* world_env = getenv("JASH_WORLD");
                    if (sid && *sid && !(world_env && world_env[0] == '0')) {
                        int depth = 3;
                        const char* depth_env = getenv("JASH_WORLD_DEPTH");
                        if (depth_env && *depth_env) depth = atoi(depth_env);
                        fs_snap = jash_fssnap_take_with_world(
                            fs_root, depth, &shared_world_scan);
                    } else {
                        fs_snap = jash_fssnap_take(fs_root);
                    }
                } else {
                    fs_root[0] = '\0';
                }
            }
        }
        {
            const char* sid = getenv("JASH_SESSION");
            if (sid && *sid && jash_env_smells_like_agent()) {
                if (shared_world_scan) {
                    g_world_delta = jash_world_check_with_scan(
                        sid, fs_root, shared_world_scan);
                } else {
                    char cwd_buf[1024];
                    if (getcwd(cwd_buf, sizeof(cwd_buf)))
                        g_world_delta = jash_world_check(sid, cwd_buf);
                }
            } else {
                jash_world_scan_free(shared_world_scan);
            }
        }

        /* ADR-0040 W2: aggregated single envelope — the OUTERMOST agent
         * invocation owns a spool file; absorbed children (JASH_ABSORBED)
         * append compact records there instead of emitting their own
         * envelope. Published once via the REAL env so every fork/exec
         * depth inherits the same path. */
        if (jash_agent_enabled() && !getenv("JASH_ABSORBED") &&
            !getenv("JASH_ABSORB_SPOOL")) {
            extern int jash_tmp_path(const char* name, char* buf, size_t cap);
            char sp[512];
            if (jash_tmp_path(".jash_absorb", sp, sizeof(sp))) {
                int ur = unlink(sp); (void)ur;   /* stale spool from a recycled pid */
                jash_agent_spool_mark_owner();   /* W3: only creator folds+unlinks */
                int sr = setenv("JASH_ABSORB_SPOOL", sp, 1); (void)sr;
                /* build_envp() feeds children from the STATE env, not the
                 * real environ — mirror it there or absorbed children
                 * never learn the spool path. */
                int er = jash_env_set(state, "JASH_ABSORB_SPOOL", sp); (void)er;
            }
        }

        /* AI mode in -c: `jash -c '? do X'` routes to AI agent (or ai.modjash if loaded).
         * Module dispatch is tried first; falls back to built-in jash_ai_route. */
        const char* cp = command_str;
        while (*cp == ' ' || *cp == '\t') cp++;
        if (cp[0] == '?') {
#ifdef JASH_TINALINUX
            code = jash_ai_route(state, token_writer, &token_wrap, cp);
#else
            const char* q_args = cp + 1;
            while (*q_args == ' ') q_args++;
            jmod_call_t mc = { state, token_writer, &token_wrap, "?", q_args };
            int mrc = jmod_dispatch("?", &mc);
            if (mrc < 0) mrc = jash_ai_route(state, token_writer, &token_wrap, cp) ? 0 : 1;
            code = mrc;
#endif
        }
        /* If the -c string spans multiple logical lines, run it through the
         * shared multi-line processor (heredocs, continuations, compound
         * accumulation) — matching script-mode semantics. Single-line
         * strings keep the fast path. F E R M I ∞ H A R T */
        else if (strchr(command_str, '\n')) {
            code = jash_run_multiline(state, command_str);
        } else {
            int handled = 0;
            /* W2: when sigil logging is on OR the Liquid Brain hook is active,
             * bypass the simple-command fast path so every command flows through
             * jash_eval_line and hits the post_cmd hooks. Costs nothing when both
             * are off. */
            int _brain_hook = jash_liquid_should_hook();
            if (jash_sigil_get() < SIGIL_LOG && !_brain_hook)
                code = eval_command_mode_fast(state, token_writer, &token_wrap,
                                              command_str, &handled);
            if (!handled)
                code = jash_eval_line(state, token_writer, &token_wrap, (char*)command_str);
        }
        state->last_status.code = code;
        double _t3 = _bench_now();

        /* ADR-0034: Record real token usage for this -c invocation. */
        {
            long cmd_tokens = (long)(strlen(command_str) + JASH_CHARS_PER_TOKEN - 1) / JASH_CHARS_PER_TOKEN;
            long output_bytes = jash_token_wrap_bytes(&token_wrap);
            jash_token_record(cmd_tokens, output_bytes, _t3 - _t2);
        }
        /* BUG-063 / ADR-0038 W5+W8: append this command + rc + duration to
         * the per-session record (no-op unless jash_record_init armed it). */
        jash_record_command(state, command_str, code,
                            (uint64_t)((_t3 - _t2) * 1e6));
        jash_run_trap_exit(state, jash_stdout_writer, 0);
        double _t4 = _bench_now();
        if (getenv("JASH_BENCH")) {
            char buf[256];
            int n = snprintf(buf, sizeof(buf),
                "state_init=%.3f eval=%.3f trap_exit=%.3f total=%.3f ms\n",
                _t1-_t0, _t3-_t2, _t4-_t3, _t4-_t0);
            WRITE_IGNORE(2, buf, n);
        }
        /* F1: persist cwd+env+aliases for the next `jash -c` in this session. */
#ifndef JASH_TINALINUX
        jash_session_save(state);
#endif
        /* ADR-0033 Wave 2: persist Liquid Reasoning Engine after every -c call. */
        jash_liquid_save();
        int agent_emit_failed = 0;
        {
            /* ADR-0038 W2: diff the tree once, feed both output paths.
             * W5: persist the delta — the impact map's raw material. */
            jash_fsdiff_t fsd;
            memset(&fsd, 0, sizeof(fsd));
            if (fs_snap) {
                jash_fssnap_diff(fs_snap, fs_root, &fsd);
                jash_fsdiff_persist(&fsd);
            }

            /* ADR-0042 F6: a backgrounded child finalizes its own record on
             * exit (state→done, rc, ms) and must NOT emit the fd-2 envelope —
             * that would land in <id>.out. The completion record lives in
             * <id>.json; the parent already returned {"job":N}. */
            const char* bg_id_env = getenv("JASH_BG_ID");
            int is_bg_child = bg_id_env && *bg_id_env;
            if (is_bg_child)
                jash_jobs_bg_finalize(atoi(bg_id_env), state->last_status.code,
                                      (long)(_t4 - _t0));

            int agent = agent_result_fd >= 0 || (jash_agent_enabled() && !is_bg_child);
            if (jash_sigil_get() >= SIGIL_LOG || agent) {
                jash_timeline_save();
                int tl_n = jash_timeline_count();
                if (agent) {
                    /* F3: machine-readable JSON envelope takes priority.
                     * ADR-0040 W2 fix: absorbed children stay silent — the
                     * outermost invocation emits the single envelope. */
                    if (agent_result_fd >= 0 || !getenv("JASH_ABSORBED")) {
                        if (agent_result_fd >= 0) {
                            agent_emit_failed = jash_agent_emit_to(state, state->last_status.code,
                                _t4-_t0, tl_n, &fsd, g_world_delta, jash_jsonw_fd_sink, &agent_result_fd) != 0;
                        } else {
                            agent_emit_failed = jash_agent_emit(state, state->last_status.code,
                                _t4-_t0, tl_n, &fsd, g_world_delta) != 0;
                        }
                    } else {
                        /* ADR-0040 W2: absorbed child → append one compact
                         * "rc ms tl" record to the outermost invocation's
                         * spool; folded into the single aggregated envelope. */
                        const char* sp = getenv("JASH_ABSORB_SPOOL");
                        if (sp && *sp) {
                            char rec[96];
                            int rn = snprintf(rec, sizeof(rec), "%d %.0f %d\n",
                                              state->last_status.code, _t4-_t0, tl_n);
                            int afd = open(sp, O_WRONLY | O_CREAT | O_APPEND, 0600);
                            if (afd >= 0) {
                                if (rn > 0) { ssize_t aw = write(afd, rec, (size_t)rn); (void)aw; }
                                close(afd);
                            }
                        }
                    }
                } else if (getenv("JASH_VERBOSE") && strcmp(getenv("JASH_VERBOSE"), "0") != 0) {
                    char summary[192];
                    int sn = snprintf(summary, sizeof(summary),
                        "[jash] rc=%d t=%.0fms tl=%d\n",
                        state->last_status.code, _t4-_t0, tl_n);
                    WRITE_IGNORE(2, summary, sn);
                }
            }
            /* ADR-0042 F2: free world-delta after emit. */
            if (g_world_delta) { free(g_world_delta); g_world_delta = NULL; }
            /* W2 text footer for non-envelope agent sessions: one stderr
             * line, ONLY when something actually changed (high signal). */
            if (!agent && fsd.valid && fsd.total > 0) {
                size_t line_cap = 96;
                for (int i = 0; i < fsd.n; i++)
                    line_cap += strlen(fsd.paths[i]) + 3;
                char* line = (char*)malloc(line_cap);
                if (line) {
                    int o = snprintf(line, line_cap, "[jash] files changed (%d):",
                                     fsd.total);
                    for (int i = 0; i < fsd.n; i++)
                        o += snprintf(line + o, line_cap - (size_t)o, " %c%s",
                                      fsd.kind[i], fsd.paths[i]);
                    if (fsd.total > fsd.n)
                        o += snprintf(line + o, line_cap - (size_t)o,
                                      " (+%d more)", fsd.total - fsd.n);
                    line[o++] = '\n';
                    WRITE_IGNORE(2, line, o);
                    free(line);
                }
            }
            /* ADR-0040 W2: adoption scorecard — one line to the REAL stderr,
             * session-scoped opportunity adoption (native/upgraded vs bypass/
             * missed); silent when the session measured no opportunity. */
            jash_adopt_scorecard_maybe();
            /* ADR-0040 W4: cross-session telemetry — one summary row per
             * governor-active session; feeds the auto-level ladder
             * (JASH_ADOPT=auto). Must run AFTER jash_agent_emit() above:
             * the absorb-spool fold stashes the counts this row reads. */
            jash_adopt_telemetry_flush();
            /* ADR-0038 W3: stale-write warning — the agent mutated file(s)
             * whose content had changed AFTER its last recorded read. The
             * most dangerous implicit assumption, checked automatically. */
            if (fsd.valid && fsd.stale_total > 0) {
                char line[768];
                int o = snprintf(line, sizeof(line),
                    "[jash] STALE-WRITE WARNING: %d file(s) you just changed "
                    "had been modified AFTER your last read of them:",
                    fsd.stale_total);
                for (int i = 0; i < fsd.n && o < (int)sizeof(line) - 8; i++)
                    if (fsd.stale[i])
                        o += snprintf(line + o, sizeof(line) - (size_t)o,
                                      " %s", fsd.paths[i]);
                o += snprintf(line + o, sizeof(line) - (size_t)o,
                    " — your view was outdated; re-read before trusting the "
                    "result (undo: rewind auto · JASH_STALE_GUARD=0 silences)\n");
                WRITE_IGNORE(2, line, o < (int)sizeof(line) ? o : (int)sizeof(line) - 1);
            }
        }
        /* ADR-0042 F5: anticipatory prefetch — learn this call's chain transition
         * and speculatively warm the next call's memoizable successors. OFF by
         * default (JASH_PREFETCH=1); no-op for prefetch children, JASH_SAFE, and
         * backgrounded (--bg) invocations. */
        if (jash_prefetch_enabled() && !getenv("JASH_BG_ID")) {
            jash_chain_entry_t pc[2];
            int pn = jash_chain_tail(pc, 2, 2);   /* newest-first */
            if (pn >= 2) jash_prefetch_learn(pc[1].cmd, pc[0].cmd);
            if (pn >= 1) jash_prefetch_fire(pc[0].cmd);
        }
        /* ADR-0043 W1: environment-drift forensics — on success record the
         * env fingerprint for (cmd, cwd); on failure name exactly what
         * changed since the last success ("it worked yesterday" solver).
         * Agent sessions only; warn-only; JASH_DRIFT=0 disables. */
        if (jash_drift_enabled() && !getenv("JASH_BG_ID"))
            jash_drift_note(command_str, state->last_status.code, NULL);
        /* The command may already have had effects: a lost result is a runtime
         * delivery failure, never success or evidence that nothing ran. */
        int final_status = agent_emit_failed ? 125 : state->last_status.code;
        if (agent_result_fd >= 0) close(agent_result_fd);
        free(cmd_processed);
        return jash_main_state_return(state, final_status);
    }

    /* ── Progressive Boot ───────────────────────────────────────────── */
    boot_loader_t boot;
    jash_boot_init(&boot, state);
    init_hist_path(state);
    unsigned hplen = 0;
    while (jash_hist_path[hplen] && hplen + 1 < sizeof(boot.hist_path))
        { boot.hist_path[hplen] = jash_hist_path[hplen]; hplen++; }
    boot.hist_path[hplen] = '\0';
    jash_boot_start_background(&boot);

    /* Script file execution */
    if (is_script_mode && script_path) {
        jash_boot_join(&boot);
        long script_fd = sys_open(script_path, 0, 0);
        if (script_fd < 0) {
            sys_write(2, "jash: ", 6);
            sys_write(2, script_path, strlen(script_path));
            sys_write(2, ": No such file or directory\n", 28);
            jash_boot_shutdown(&boot);
            return jash_main_state_return(state, 127);
        }
        /* JINV-007 — um script grande demais NÃO pode ser executado pela
         * metade e reportado como sucesso.
         *
         * O buffer era fixo em 64 KiB: passando disso, `65536 - script_len - 1`
         * chegava a 0, o `sys_read` devolvia 0, o laço terminava e o script
         * TRUNCADO era executado, com exit 0. Reproduzido em 2026-07-27 com um
         * script de 70.021 bytes: o bash chega ao fim, o jash executava só o
         * começo e dizia sucesso. Para um shell de agente isso é a pior classe
         * de defeito — o LLM gera um script longo, metade roda, e o rc=0 diz
         * que deu tudo certo.
         *
         * Correção: crescer o buffer conforme lê (dobrando), sem teto artificial.
         * Falha de alocação vira erro ALTO (rc 127), nunca execução parcial.
         * JINV-068: esse laço virou jash_slurp_program(), compartilhado com o
         * leitor de stdin — a mesma garantia nas duas portas de entrada.
         *
         * LIMITE RESIDUAL CONHECIDO (medido, não corrigido aqui): acima de
         * ~128 KiB o script ainda falha, agora no expansor
         * (JASH_PARAM_EXPAND_CAP = 131072, c/parser/param_expand.c). A
         * diferença que importa: ele falha ALTO (rc 127), não em silêncio com
         * rc 0. Subir esse segundo teto exige mexer no param_expand — que tem
         * um heap-overflow confirmado por ASan (auditoria 2026-07-27, achado
         * 12.1) e deve ser tratado junto, não isolado.
         * Gate: tests/test_jinv007_script_size.sh
         */
        char* script_buf = jash_slurp_program(script_fd, NULL);
        sys_close(script_fd);
        if (!script_buf) {                                /* falha ALTA */
            sys_write(2, "jash: ", 6);
            sys_write(2, script_path, strlen(script_path));
            sys_write(2, ": script grande demais para a memoria disponivel\n", 48);
            jash_boot_shutdown(&boot);
            return jash_main_state_return(state, 127);
        }

        const char* sp = script_buf;
        if (sp[0] == '#' && sp[1] == '!') {
            while (*sp && *sp != '\n') sp++;
            if (*sp == '\n') sp++;
        }

        /* BUG-112: `jash script.sh a b c` — argv after the script path are the
         * script's positional parameters ($1..$9; $0 = script path). They were
         * silently dropped, so every script taking arguments broke. */
        if (script_argi >= 0) {
            int supplied = argc - (script_argi + 1);
            if (supplied > JASH_POS_MAX) {
                fprintf(stderr,
                        "jash: script: %d positional parameters exceed limit %d — refusing\n",
                        supplied, JASH_POS_MAX);
                free(script_buf);
                jash_boot_shutdown(&boot);
                return jash_main_state_return(state, 2);
            }
            if (jash_env_set(state, "0", script_path) != 0 ||
                jash_env_replace_positionals(
                    state, (const char* const*)&argv[script_argi + 1], supplied) != 0) {
                fprintf(stderr,
                        "jash: script: positional storage failed — refusing\n");
                free(script_buf);
                jash_boot_shutdown(&boot);
                return jash_main_state_return(state, 2);
            }
        }

        jash_run_multiline(state, sp);
        jash_run_trap_exit(state, jash_stdout_writer, 0);
        /* ADR-0050: the -c path saved the journal at exit; script mode never
         * did, so mutations from a script were invisible to the next call
         * (rewind/undo after a script saw an empty timeline). Same gate. */
        if (jash_sigil_get() >= SIGIL_LOG || jash_agent_enabled())
            jash_timeline_save();
        jash_boot_shutdown(&boot);
        free(script_buf);
        int final_status = state->last_status.code;
        return jash_main_state_return(state, final_status);
    }

    /* ══ JINV-068 — stdin não-tty é UM PROGRAMA, não uma fila de linhas ════
     *
     * Aqui embaixo mora a REPL, que lê UMA linha e avalia UMA linha. Quando
     * stdin não é um terminal (`jash < script.sh`, `curl … | jash`, um
     * heredoc), essa disciplina destrói toda construção multi-linha: a
     * primeira linha `if true` chega sozinha ao avaliador, que responde
     * "syntax error: if requires 'then'". Medido, com o MESMO programa:
     *
     *   printf 'if true\nthen\necho OK\nfi\n' > /tmp/s.sh
     *   jash /tmp/s.sh              → OK        (rc 0)
     *   printf … | jash             → syntax error (rc 2)   ← mesmo programa
     *
     * O mesmo valia para for/while/until/case/função/heredoc. Custo medido no
     * corpus do Oils: 148 casos, ~5 pontos percentuais
     * (audits/BENCHMARK-compatibilidade-externa.md §3.4). E `curl … | sh` é
     * como metade do mundo instala software.
     *
     * O conserto NÃO é um segundo acumulador: é usar o caminho do ARQUIVO,
     * que já resolve isto — jash_slurp_program() lê o programa inteiro e
     * jash_run_multiline() faz a acumulação de construções compostas,
     * heredocs e continuações. Uma implementação, duas portas de entrada.
     * (Duas implementações da mesma coisa foi o pior achado da auditoria de
     * 2026-07-27: 4 casadores de glob, 7 rtrims cegos.)
     *
     * Condição: `!isatty(0)`. Um terminal continua na REPL linha a linha —
     * é ela que dá prompt, histórico e edição. `-i` força a REPL mesmo com
     * stdin redirecionado (Beyond /init).
     *
     * RESÍDUO CONHECIDO, medido: um `read` dentro de um programa vindo do
     * pipe agora vê EOF, porque o programa inteiro já foi consumido. No bash
     * o `read` rouba a próxima linha do próprio script. É a troca clássica
     * desta correção; escolhida porque construção multi-linha quebrada é o
     * caso comum e `read` de um `curl | sh` é o caso patológico (por isso
     * instaladores reais leem de /dev/tty).
     *
     * Gate: tests/test_jinv068_stdin_multiline.sh */
    if (!force_interactive
#ifdef JASH_TARGET_MINIX
        && !jash_surface_active
#endif
#ifdef JASH_BETHOS
        && !jash_bethos_surface_active()
#endif
        && !isatty(STDIN_FILENO)) {
        jash_boot_join(&boot);
        char* prog = jash_slurp_program(STDIN_FILENO, NULL);
        if (!prog) {                                      /* falha ALTA */
            sys_write(2, "jash: stdin: programa grande demais para a "
                         "memoria disponivel\n", 60);
            jash_boot_shutdown(&boot);
            return jash_main_state_return(state, 127);
        }
        const char* pp = prog;
        if (pp[0] == '#' && pp[1] == '!') {   /* mesmo tratamento do arquivo */
            while (*pp && *pp != '\n') pp++;
            if (*pp == '\n') pp++;
        }
        jash_run_multiline(state, pp);
        jash_run_trap_exit(state, jash_stdout_writer, 0);
        if (jash_sigil_get() >= SIGIL_LOG || jash_agent_enabled())
            jash_timeline_save();   /* ADR-0050: same journal discipline as -c */
        jash_boot_shutdown(&boot);
        free(prog);
        int final_status = state->last_status.code;
        return jash_main_state_return(state, final_status);
    }

    /* No evaluator fork may race the progressive loader: malloc/stdio locks
     * inherited from another thread have no owner in the child. */
    jash_boot_join(&boot);

    /* REPL. */
    char line[1024];
    
    /* Initialize AI mode once before entering REPL (not lazily) */
    jash_ai_init();
    
    for (;;) {
        /* PROMPT_COMMAND: execute before each prompt (bash parity) */
        const char* prompt_cmd = jash_env_get(state, "PROMPT_COMMAND");
        if (prompt_cmd && *prompt_cmd) {
            jash_eval_line(state, jash_highlighted_stdout_writer, 0, (char*)prompt_cmd);
        }
        int rc;
#ifdef JASH_TARGET_MINIX
        if (jash_surface_active) {
            int prompt_rc = jash_minix_surface_prompt(jash_surface_active);
            if (prompt_rc < 0) break;
            rc = jash_minix_surface_readline(jash_surface_active, line, sizeof(line));
        } else
#endif
        {
            rc = jash_read_line(state,
                                jash_stdin_reader, 0,
                                jash_stdout_writer, 0,
                                line, sizeof(line));
        }
        if (rc == -1) {
            /* EOF — emit goodbye only if interactive; piped/scripted mode
             * should exit silently so output is clean. */
            if (state->interactive)
                jash_stdout_writer(0, "\nbye.\n", 6);
            break;
        }
        if (rc < 0) {
#ifdef JASH_TARGET_MINIX
            if (jash_surface_active) break;
#endif
            continue;
        }

        const char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) {
#ifdef JASH_TARGET_MINIX
            if (jash_surface_active &&
                jash_minix_surface_receipt(jash_surface_active, 0) < 0)
                break;
#endif
            continue;
        }

        if (state->interactive && repl_is_plain_exit(p) && !repl_confirm_exit()) {
            continue;
        }

        ensure_repl_features(state);

        /* AI mode: '?' is routed through module dispatch first (ADR-0003 M2),
         * falling back to the built-in jash_ai_route when no module claims it. */
        if (p[0] == '?') {
#ifdef JASH_TINALINUX
            int handled = jash_ai_route(state, jash_stdout_writer, 0, p);
            state->last_status.code = handled;
            if (!state->interactive && state->jobs) jash_job_poll(state->jobs);
#ifdef JASH_TARGET_MINIX
            if (jash_surface_active && jash_minix_surface_receipt(
                    jash_surface_active, state->last_status.code) < 0)
                break;
#endif
            continue;
#else
            const char* q_args = p + 1;
            while (*q_args == ' ') q_args++;
            jmod_call_t mc = { state, jash_stdout_writer, 0, "?", q_args };
            int handled = jmod_dispatch("?", &mc);
            if (handled < 0) handled = jash_ai_route(state, jash_stdout_writer, 0, p) ? 0 : -1;
            if (handled >= 0) {
                state->last_status.code = handled;
                if (!state->interactive) { if (state->jobs) jash_job_poll(state->jobs); }
#ifdef JASH_TARGET_MINIX
                if (jash_surface_active && jash_minix_surface_receipt(
                        jash_surface_active, state->last_status.code) < 0)
                    break;
#endif
                continue;
            }
#endif
        }

        int heredoc_rewrite_failed = 0;
        char heredoc_tmp_paths[8][256];
        int heredoc_tmp_fds[8];
        unsigned heredoc_tmp_count = 0;
        {
            const char* hd = jash_find_unquoted_heredoc_op(line, line);
            if (hd && hd[2] != '(' && hd[2] != '<') {
                int indented = (hd[2] == '-');
                const char* delim_start = hd + 2 + indented;
                while (*delim_start == ' ' || *delim_start == '\t') delim_start++;
                char delim[256];
                /* JINV-092: a palavra delimitadora é lida pela MESMA rotina do
                 * jash_run_multiline — citação removida, `<<'EOF'`/`<<"EOF"`/
                 * `<<\EOF` todos fecham numa linha `EOF` nua. A leitura local
                 * copiava as aspas JUNTO ('EOF'), o fechamento nunca casava e
                 * o corpo inteiro ia parar no avaliador como comando (BUG-061:
                 * crases no corpo de heredoc citado EXECUTAVAM). */
                jash_heredoc_delim(delim_start, delim, sizeof(delim), NULL);
                size_t di = strlen(delim);

                if (di > 0) {
                    /* BUG-061: o readline deixa o STDIN em O_NONBLOCK; um
                     * EAGAIN aqui era tratado como EOF, a captura devolvia
                     * NULL, a linha ia ao avaliador SEM corpo e as linhas do
                     * corpo voltavam ao readline como COMANDO — crases nelas
                     * EXECUTAVAM (injeção por temporização). Quem decidiu
                     * capturar ESPERA o dado: bloqueia até o delimitador ou
                     * EOF de verdade, como o bash no PS2. */
                    int fl = fcntl(STDIN_FILENO, F_GETFL, 0);
                    if (fl >= 0 && (fl & O_NONBLOCK))
                        fcntl(STDIN_FILENO, F_SETFL, fl & ~O_NONBLOCK);
                    char* content = indented ?
                        jash_parse_heredoc_ex(delim, 1) :
                        jash_parse_heredoc(delim);
                    if (fl >= 0 && (fl & O_NONBLOCK))
                        fcntl(STDIN_FILENO, F_SETFL, fl);

                    if (content) {
                        char tmpname[256]; snprintf(tmpname, sizeof(tmpname), "%s/jash_heredoc_XXXXXX", jash_tmpdir());
                        int tmpfd = mkstemp(tmpname);
                        if (tmpfd >= 0) {
                            fcntl(tmpfd, F_SETFD, FD_CLOEXEC);
                            int bodyfd = dup(tmpfd);
                            if (bodyfd >= 0) fcntl(bodyfd, F_SETFD, FD_CLOEXEC);
                            size_t clen = 0, written = 0;
                            while (content[clen]) clen++;
                            int materialized = bodyfd >= 0;
                            while (materialized && written < clen) {
                                ssize_t n = write(tmpfd, content + written, clen - written);
                                if (n < 0 && errno == EINTR) continue;
                                if (n <= 0) { materialized = 0; break; }
                                written += (size_t)n;
                            }
                            if (bodyfd < 0) {
                                /* Keep the owned descriptor for identity-safe
                                 * cleanup; no body bytes were written. */
                                bodyfd = tmpfd;
                            } else if (close(tmpfd) != 0) {
                                materialized = 0;
                            }

                            if (!materialized) {
                                fprintf(stderr,
                                        "jash: heredoc: failed to materialize body -- refusing\n");
                                (void)repl_cleanup_heredoc(tmpname, bodyfd);
                                state->last_status.code = 2;
                                heredoc_rewrite_failed = 1;
                            } else if (repl_preflight_heredoc(tmpname, bodyfd) != 0) {
                                fprintf(stderr,
                                        "jash: heredoc: safe cleanup unavailable -- refusing before execution\n");
                                (void)repl_cleanup_heredoc(tmpname, bodyfd);
                                state->last_status.code = 2;
                                heredoc_rewrite_failed = 1;
                            } else {

                                char new_line[2048];
                                unsigned nlen = 0;
                                const char* src = line;
                                while (src < hd && nlen < sizeof(new_line) - 32)
                                    new_line[nlen++] = *src++;
                                new_line[nlen++] = ' ';
                                new_line[nlen++] = '<';
                                new_line[nlen++] = ' ';
                                size_t qlen = jash_heredoc_quote_path(tmpname,
                                    new_line + nlen, sizeof(new_line) - nlen);
                                nlen += (unsigned)qlen;

                                if (!qlen || nlen >= sizeof(line)) {
                                    fprintf(stderr,
                                            "jash: heredoc: rewritten command too long -- refusing\n");
                                    (void)repl_cleanup_heredoc(tmpname, bodyfd);
                                    state->last_status.code = 2;
                                    heredoc_rewrite_failed = 1;
                                } else if (heredoc_tmp_count >=
                                           sizeof heredoc_tmp_paths / sizeof heredoc_tmp_paths[0]) {
                                    fprintf(stderr,
                                            "jash: heredoc: too many materialized bodies -- refusing\n");
                                    (void)repl_cleanup_heredoc(tmpname, bodyfd);
                                    state->last_status.code = 2;
                                    heredoc_rewrite_failed = 1;
                                } else {
                                    snprintf(heredoc_tmp_paths[heredoc_tmp_count],
                                             sizeof heredoc_tmp_paths[heredoc_tmp_count],
                                             "%s", tmpname);
                                    heredoc_tmp_fds[heredoc_tmp_count] = bodyfd;
                                    heredoc_tmp_count++;
                                    memcpy(line, new_line, nlen + 1);
                                }
                            }
                        } else {
                            fprintf(stderr,
                                    "jash: heredoc: failed to create body file -- refusing\n");
                            state->last_status.code = 2;
                            heredoc_rewrite_failed = 1;
                        }
                        free(content);
                    } else {
                        fprintf(stderr,
                                "jash: heredoc: failed to capture body -- refusing\n");
                        state->last_status.code = 2;
                        heredoc_rewrite_failed = 1;
                    }
                }
            }
        }
        if (heredoc_rewrite_failed) {
            for (unsigned i = 0; i < heredoc_tmp_count; i++)
                (void)repl_cleanup_heredoc(heredoc_tmp_paths[i],
                                           heredoc_tmp_fds[i]);
#ifdef JASH_TARGET_MINIX
            if (jash_surface_active && jash_minix_surface_receipt(
                    jash_surface_active, state->last_status.code) < 0)
                break;
#endif
            continue;
        }

        jash_history_push(state, line);

        /* Tear down the statusbar's scroll region BEFORE running the
         * command. TUI apps (opencode, BearCode, htop, vim, etc.) expect
         * full terminal control — without this they draw into the lines-2-N
         * scroll window and the cursor jumps around line 1 where the
         * statusbar lives, producing the "dark / monochrome / weird" look
         * the user sees. Statusbar re-inits automatically when the next
         * prompt readline starts. F E R M I ∞ H A R T */
        jash_statusbar_teardown_terminal();

        if (g_suggestions_engine) {
            uint64_t start_time = 0;
            struct timespec ts;
            if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
                start_time = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;

            int code = jash_eval_line(state,
                                      jash_highlighted_stdout_writer, 0,
                                      line);
            
            struct timespec ts2;
            uint64_t end_time = 0;
            if (clock_gettime(CLOCK_MONOTONIC, &ts2) == 0)
                end_time = (uint64_t)ts2.tv_sec * 1000000000ULL + (uint64_t)ts2.tv_nsec;
            uint64_t duration = start_time && end_time >= start_time ? end_time - start_time : 0;
            
            jash_ss_history_record(g_suggestions_engine, line, code, duration);
        } else {
            jash_eval_line(state,
                           jash_highlighted_stdout_writer, 0,
                           line);
        }
        int heredoc_cleanup_failed = 0;
        for (unsigned i = 0; i < heredoc_tmp_count; i++)
            if (repl_cleanup_heredoc(heredoc_tmp_paths[i],
                                     heredoc_tmp_fds[i]) != 0)
                heredoc_cleanup_failed = 1;
        if (heredoc_cleanup_failed) state->last_status.code = 2;
#ifdef JASH_TARGET_MINIX
        if (jash_surface_active && jash_minix_surface_receipt(
                jash_surface_active, state->last_status.code) < 0)
            break;
#endif

        /* Reset only the INVISIBLE terminal modes a child may have left
         * set. Critically we do NOT toggle alt-screen (ESC[?1049l) here:
         * emitting it after every command — even `echo` or `ls` — makes
         * the terminal restore/redraw the normal buffer and the screen
         * appears to jump. Apps that use alt-screen restore it themselves
         * on clean exit; the rare Ctrl+C-in-alt-screen case is handled by
         * the conditional startup heal instead.
         *
         * What we DO reset (all zero visual side effects, just fix stuck
         * input/render state left by a TUI that died mid-flight):
         *   ESC(B    G0 charset → US-ASCII   (fixes the `pppppp` glyphs:
         *            DEC line-drawing left active maps p/q/x/etc. to box
         *            chars, so typed text was unreadable)
         *   ESC)B    G1 charset → US-ASCII
         *   ESC[?1l  normal (not application) cursor keys → arrows work
         *   ESC[?25h show cursor (a TUI may have hidden it)
         *   ESC[0m   reset SGR (clears stuck color/bold/reverse) */
        if (state->interactive) {
            static const char term_reset[] =
                "\033(B\033)B\033[?1l\033[?25h\033[0m";
            WRITE_IGNORE(STDOUT_FILENO, term_reset, sizeof(term_reset) - 1);
        }

        /* `-c` and script-file paths exit above (early return). Stdin-fed
         * non-interactive sessions (e.g. `echo cmd | jash`) must keep
         * looping until the reader returns EOF — breaking on
         * `!state->interactive` here would stop after the FIRST line. */

        /* The `exit` builtin signals shutdown by setting this flag. Both
         * interactive and stdin-piped runs must honor it — otherwise typing
         * `exit` leaves the user stranded in the REPL. */
        if (state->exit_requested) break;

        if (state->jobs) jash_job_poll(state->jobs);
    }

    jash_run_trap_exit(state, jash_stdout_writer, 0);

    jash_persist_save(state);
    jash_history_save(state, jash_hist_path);
    
    if (g_highlighter) {
        jash_highlight_flush(g_highlighter, jash_stdout_writer, NULL);
        jash_highlighter_free(g_highlighter);
        g_highlighter = NULL;
    }
    if (g_progress_bar) {
        jash_progress_destroy(g_progress_bar);
        g_progress_bar = NULL;
    }
    if (g_suggestions_engine) {
        jash_ss_engine_persist(g_suggestions_engine);
        jash_ss_engine_destroy(g_suggestions_engine);
        g_suggestions_engine = NULL;
    }
    if (g_editor_session) {
        jash_editor_session_destroy(g_editor_session);
        g_editor_session = NULL;
    }

    /* ADR-0033 Wave 2: persist Liquid Reasoning Engine weights + state. */
    jash_liquid_cleanup();

    /* JINV-103: cleanup durable memory before exit */
    jash_brain_cleanup();

    jash_boot_shutdown(&boot);

    int final_status = state->last_status.code;
    return jash_main_state_return(state, final_status);
}
