// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// 🎵 Hear F E R M I ∞ H A R T music. No obligation. Just a wish.
//
// Module: readline/loop — read-edit-render loop (impl).
//
// Integrated features:
//   - Auto-pair (brackets, quotes, parentheses)
//   - Command Palette (Ctrl+P)
//   - Bracket matching visual
//   - Theme-aware rendering
//   - Session recording

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#if !defined(JASH_TARGET_BEYOND) || defined(JASH_SURFACE_TERMIOS)
#include <termios.h>
#endif
#include <unistd.h>
#include "sys/syscalls.h"
#ifdef JASH_TINALINUX
#include <osif/tina/surface.h>
#endif
#ifdef JASH_BETHOS
#include <osif/bethos/surface.h>
#endif
#ifndef JASH_TARGET_BEYOND
#include <sys/ioctl.h>
#endif
#include "loop.h"
#include "edit.h"
#include "render.h"
#include "keymap.h"
#include "balance.h"
#include "complete.h"
#include "jux.h"
#include "autopair.h"
#include "palette.h"
#include "bracket.h"
#include <core/history.h>
#include <core/record.h>
#include <core/env.h>
#include <easter/bear_egg.h>
#include <theme/theme.h>
#include <theme/io.h>
#include <theme/prompt.h>
#include <ui/statusbar.h>
#include <core/jash_ai.h>
#include <core/agent_json.h>
#include <completion/smart_suggestions.h>

typedef struct {
    int active;
    uint32_t hist_idx;
    char draft[JASH_LINE_MAX];
    uint32_t draft_len;
} histnav_t;

static void hn_save_draft(histnav_t* hn, const jash_line_t* L) {
    if (hn->draft_len) return;
    hn->draft_len = L->len;
    for (uint32_t i = 0; i < L->len && i < JASH_LINE_MAX; i++)
        hn->draft[i] = L->buf[i];
}
static void hn_load_to_line(jash_state_t* s, histnav_t* hn, jash_line_t* L) {
    (void)s;
    L->len = 0; L->cur = 0;
    const uint32_t total = jash_history_count(s);
    if (hn->hist_idx < total) {
        const char* h = jash_history_at(s, hn->hist_idx);
        if (h) { while (*h && L->len < JASH_LINE_MAX) L->buf[L->len++] = *h++; }
    } else if (hn->draft_len) {
        for (uint32_t i = 0; i < hn->draft_len && i < JASH_LINE_MAX; i++)
            L->buf[L->len++] = hn->draft[i];
    }
    L->cur = L->len;
}

static void copy_out(const jash_line_t* L, char* out, uint32_t cap) {
    uint32_t n = L->len < cap - 1 ? L->len : cap - 1;
    for (uint32_t i = 0; i < n; i++) out[i] = L->buf[i];
    out[n] = '\0';
}

static int g_first_render = 1;  /* Set on readline entry, cleared on first render_prompt */
static int g_tty_suggest = 0;   /* isatty(STDIN)&&isatty(STDOUT), captured once on
                                 * readline entry — 2 ioctls per render was per-key
                                 * waste; the fds can't change tty-ness mid-session. */

/* Buffered single-flush writer for prompt redraws. Without this, one
 * render_prompt() pass fires 10-20+ separate unbuffered write(2) syscalls
 * (jash_writes/jash_writef call the real writer once per literal segment,
 * and jash_stdout_writer is a bare sys_write per call) to redraw the SAME
 * on-screen line: clear, prompt segments, cursor repositioning. Terminals
 * and multiplexers that don't coalesce those into one frame can paint the
 * partial state, which reads as flicker — most noticeable during the
 * 250ms-cadence idle spinner tick (loop below) since it redraws on a timer
 * even though only the spinner glyph changed, but it happens on every
 * keystroke too. Collecting the whole pass into one buffer and flushing
 * with a single write() makes each redraw atomic from the terminal's POV.
 * F E R M I ∞ H A R T */
#define RENDER_FLUSH_CAP 8192
static char render_flush_buf[RENDER_FLUSH_CAP];
static size_t render_flush_len = 0;
static jash_writer_t render_flush_real_w;
static void* render_flush_real_ctx;

static long render_flush_writer(void* ctx, const char* buf, size_t n) {
    (void)ctx;
    if (render_flush_len >= sizeof(render_flush_buf)) return (long)n;  /* drop, buffer full */
    size_t room = sizeof(render_flush_buf) - render_flush_len;
    if (n > room) n = room;
    memcpy(render_flush_buf + render_flush_len, buf, n);
    render_flush_len += n;
    return (long)n;
}

static void render_flush_begin(jash_writer_t w, void* ctx) {
    render_flush_real_w = w;
    render_flush_real_ctx = ctx;
    render_flush_len = 0;
}

static void render_flush_end(void) {
    if (render_flush_len > 0 && render_flush_real_w)
        render_flush_real_w(render_flush_real_ctx, render_flush_buf, render_flush_len);
    render_flush_len = 0;
}

#ifdef JASH_BETHOS
static void bethos_prompt_append(char *out, size_t cap, size_t *len,
                                 const char *text)
{
    size_t i = 0u;
    if (!out || !len || !text || *len >= cap) return;
    while (text[i] && *len + 1u < cap) out[(*len)++] = text[i++];
}

static void bethos_prompt(jash_state_t *s, jash_writer_t w, void *ctx)
{
    /* Colored standard prompt without the expensive per-prompt Git probe.
     * Format matches jash_prompt_render (user@host:path) but skips
     * find_git_root/get_git_branch per render, which costs VFS syscalls
     * in the guest on every keystroke. Colors are content styling inside
     * the typed PROMPT cell, not a TTY. */
    char prompt[768];
    size_t length = 0u;
    const char *user = jash_env_get(s, "USER");
    const char *cwd = s->cwd[0] ? s->cwd : "/";
    const jash_theme_t *theme = jash_theme_current();
    char status[32];

    if (!user) user = "fermi";
    bethos_prompt_append(prompt, sizeof(prompt), &length, theme->role_user);
    bethos_prompt_append(prompt, sizeof(prompt), &length, user);
    bethos_prompt_append(prompt, sizeof(prompt), &length, "\033[0m");
    bethos_prompt_append(prompt, sizeof(prompt), &length, theme->role_accent);
    bethos_prompt_append(prompt, sizeof(prompt), &length, "@");
    bethos_prompt_append(prompt, sizeof(prompt), &length, "\033[0m");
    bethos_prompt_append(prompt, sizeof(prompt), &length, theme->role_host);
    bethos_prompt_append(prompt, sizeof(prompt), &length, JASH_HOSTNAME);
    bethos_prompt_append(prompt, sizeof(prompt), &length, "\033[0m:");
    bethos_prompt_append(prompt, sizeof(prompt), &length, theme->role_path);
    bethos_prompt_append(prompt, sizeof(prompt), &length, cwd);
    bethos_prompt_append(prompt, sizeof(prompt), &length, "\033[0m");
    if (s->last_status.code != 0) {
        snprintf(status, sizeof(status), "[%d]", s->last_status.code);
        bethos_prompt_append(prompt, sizeof(prompt), &length, theme->role_err);
        bethos_prompt_append(prompt, sizeof(prompt), &length, status);
        bethos_prompt_append(prompt, sizeof(prompt), &length, "\033[0m");
    }
    bethos_prompt_append(prompt, sizeof(prompt), &length, " ");
    bethos_prompt_append(prompt, sizeof(prompt), &length, theme->role_sigil);
    bethos_prompt_append(prompt, sizeof(prompt), &length, "\033[1m$");
    bethos_prompt_append(prompt, sizeof(prompt), &length, "\033[0m ");
    if (length == 0u) return;
    if (jash_bethos_surface_prompt(prompt, length) != 0)
        w(ctx, prompt, length);
}
#endif

static void render_prompt(jash_state_t* s, jash_writer_t w, void* ctx, const jash_line_t* L) {
    char sug[JASH_LINE_MAX];
    sug[0] = 0;
    if (g_tty_suggest)
        jash_jux_autosuggest(s, L, sug, sizeof(sug));

    render_flush_begin(w, ctx);

    /* Render status bar first (on last line). NOTE: jash_statusbar_render
     * writes its composed frame directly via the real fd (with its own
     * dirty-check dedup), bypassing the writer param entirely — already
     * a single atomic syscall on its own, so buffering doesn't apply here. */
    jash_statusbar_render(render_flush_writer, ctx, s);

    /* Render corner indicators in top-right if statusbar is disabled.
     * Same as above: these write directly to the real fd, not through `w`. */
    render_ai_mode_indicator();
    render_sigil_indicator();

    /* Ensure the prompt starts on a fresh line after command output/banner.
     * The previous code repositioned the cursor absolutely when the statusbar
     * was on, which could land inside the just-printed output and erase it.
     * Let the prompt land naturally on the current line; the statusbar owns
     * row 1 via the scroll region independently. */
    if (g_first_render) {
        jash_writes(render_flush_writer, ctx, "\r\n");
        g_first_render = 0;
    }

    jash_render_line_suggest(render_flush_writer, ctx, s, L, sug);

    render_flush_end();
}

static char accum[JASH_LINE_MAX * 4];
static int  accum_len = 0;

static void reset_accum(void) { accum_len = 0; accum[0] = 0; }

/* Append n bytes to the multi-line accumulator. Returns 0 if it would
 * overflow (caller then stops accumulating and runs what it has). */
static int accum_append(const char* s, int n) {
    if (n < 0) n = 0;
    if (accum_len + n >= (int)sizeof(accum) - 1) return 0;
    for (int i = 0; i < n; i++) accum[accum_len++] = s[i];
    accum[accum_len] = '\0';
    return 1;
}

/* 1 if the text has an unclosed single/double quote (backslash escapes the
 * next byte). Used to decide multi-line quote continuation. */
static int quotes_open(const char* s, int n) {
    char q = 0;
    for (int i = 0; i < n; i++) {
        if (s[i] == '\\') { i++; continue; }
        if (q) { if (s[i] == q) q = 0; }
        else if (s[i] == '\'' || s[i] == '"') q = s[i];
    }
    return q != 0;
}

/* ---- terminal raw mode ----
 * Termios is captured ONCE at startup. Without this, a TUI child (opencode,
 * vim, htop) that died mid-flight via Ctrl+C left termios in raw + no-echo;
 * jash's next enable_raw_mode then re-saved that broken state as "original",
 * and on the next disable_raw_mode restored to broken — user couldn't see
 * what they typed at the next external command.  F E R M I ∞ H A R T */
#if !defined(JASH_TARGET_BEYOND) || defined(JASH_SURFACE_TERMIOS)
static struct termios g_orig_termios;
static int g_orig_saved = 0;
static int g_raw_mode = 0;

static void disable_raw_mode(void) {
    if (g_orig_saved) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig_termios);
        g_raw_mode = 0;
    }
}

static int enable_raw_mode(void) {
    if (!isatty(STDIN_FILENO)) return -1;
    if (!g_orig_saved) {
        if (tcgetattr(STDIN_FILENO, &g_orig_termios) < 0) return -1;
        g_orig_saved = 1;
        atexit(disable_raw_mode);
    }

    struct termios raw = g_orig_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;      /* Don't block */
    raw.c_cc[VTIME] = 1;     /* 100ms timeout */

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) < 0) return -1;
    g_raw_mode = 1;
    return 0;
}
#else
/* Beyond: no termios. Stubs so the rest of the file compiles. */
static void disable_raw_mode(void) { }
static int enable_raw_mode(void) { return -1; }
#endif

int jash_read_line(jash_state_t* s,
                   jash_reader_t r, void* rctx,
                   jash_writer_t w, void* wctx,
                   char* out, uint32_t out_cap) {
    if (!s || !r || !w || !out || out_cap < 2) return -2;

    /* Non-interactive fast path: stdin is a pipe/file, NOT a tty. The full
     * interactive readline (raw mode, statusbar, render_prompt, keystroke echo)
     * is pure noise here — it both pollutes stdout (every keystroke echoed
     * back, ~30x amplification) and tanks throughput. Just read a line of
     * bytes from stdin and return.
     *
     * The previous code unconditionally entered the interactive engine,
     * which is why `echo cmd | jash` mirrored every input character back
     * to stdout and ran the statusbar redraw loop. F E R M I ∞ H A R T */
    if (!s->interactive) {
        uint32_t i = 0;
        char b;
#ifdef JASH_TARGET_BEYOND
        /* Beyond serial input: kernel delivers raw bytes, no line discipline.
         * We must echo printable characters and handle backspace/Enter/Ctrl-C
         * manually. A simple "$ " prompt is shown before each command.
         * F E R M I ∞ H A R T */
        {
#ifdef JASH_BETHOS
            bethos_prompt(s, w, wctx);
#else
            static const char prompt[] = "$ ";
            w(wctx, prompt, 2);
#endif
        }
#endif
        for (;;) {
            int rc = r(rctx, &b);
#ifdef JASH_TARGET_BEYOND
            /* On Beyond, rc==2 means EINTR (signal delivery from kernel).
             * Treat as Ctrl-C: cancel current line, show new prompt. */
            if (rc == 2) {
                w(wctx, "\r\n", 2);
                i = 0;
                {
#ifdef JASH_BETHOS
                    bethos_prompt(s, w, wctx);
#else
                    static const char prompt[] = "$ ";
                    w(wctx, prompt, 2);
#endif
                }
                continue;
            }
#endif
            if (rc <= 0) {
                out[i] = '\0';
                return (i == 0) ? -1 : (int)i;  /* -1 = EOF, else byte count */
            }
            if (b == '\n' || b == '\r') {
#ifdef JASH_TARGET_BEYOND
                w(wctx, "\r\n", 2);
#endif
                out[i] = '\0';
                return (int)i;
            }
#ifdef JASH_TARGET_BEYOND
            /* Beyond serial: handle special characters manually */
            if (b == 0x03) {  /* Ctrl-C */
                w(wctx, "^C\r\n", 4);
                i = 0;
                {
#ifdef JASH_BETHOS
                    bethos_prompt(s, w, wctx);
#else
                    static const char prompt[] = "$ ";
                    w(wctx, prompt, 2);
#endif
                }
                continue;
            }
            if (b == 0x04) {  /* Ctrl-D (EOF) */
                if (i == 0) {
                    out[0] = '\0';
                    return -1;  /* EOF */
                }
                continue;  /* ignore Ctrl-D mid-line */
            }
            if (b == 0x08 || b == 0x7F) {  /* Backspace / DEL */
                if (i > 0) {
                    i--;
                    w(wctx, "\b \b", 3);  /* erase character on serial */
                }
                continue;
            }
            if (b >= 0x20 && b < 0x7F) {  /* Printable */
                if (i + 1 < out_cap) {
                    out[i++] = b;
                    w(wctx, &b, 1);  /* echo character back */
                }
                continue;
            }
            /* Other control chars: ignore */
            continue;
#else
            if (i + 1 < out_cap) out[i++] = b;
            /* Silently truncate over-long lines (matches POSIX read semantics
             * better than dropping the line entirely). */
#endif
        }
    }

    /* Conditional heal: only repair the terminal if the inherited termios
     * looks broken (no echo + non-canonical = a TUI ancestor died without
     * restoring). Avoids the side effect where an unconditional reset
     * forces the banner / bear-easter into view on every fresh jash. */
#ifndef JASH_TARGET_BEYOND
    {
        struct termios cur;
        if (tcgetattr(STDIN_FILENO, &cur) == 0) {
            int echo_off = !(cur.c_lflag & ECHO);
            int icanon_off = !(cur.c_lflag & ICANON);
            if (echo_off && icanon_off) {
                /* Inherited a TUI's leftover state. Repair charsets, modes,
                 * cursor, SGR — minimum needed to make subsequent typing
                 * visible. */
                static const char term_reset[] =
                    "\033(B\033)B\033>\033[?25h\033[?1049l\033[?1l\033[?5l"
                    "\033[?7h\033[0m";
                WRITE_IGNORE(STDOUT_FILENO, term_reset, sizeof(term_reset) - 1);
            }
        }
    }
#endif

    enable_raw_mode();
    jash_jux_load_config(s);
    jash_theme_init(s);
    jash_autopair_init();
    jash_statusbar_init(s);
    /* jash_ai_init() moved to shell initialization to preserve AI mode state */
    g_first_render = 1;
    g_tty_suggest = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);

    jash_line_t L;
    jash_line_reset(&L);

    jash_keydec_t dec;
    jash_keydec_init(&dec);

    histnav_t hn;
    hn.active = 0;
    hn.hist_idx = jash_history_count(s);
    hn.draft_len = 0;

    reset_accum();
    int tab_after_suggestion = 0;
    int vi_cmd_mode = 0;   /* 0 = insert, 1 = normal/command */
    int vi_pending  = 0;   /* pending operator: 'd' for dd, 'r' for r<char> */
    int paste_mode = 0;
    int paste_changed = 0;

    /* Initial paint — PS1 */
    render_prompt(s, w, wctx, &L);

    /* Timer for status bar updates */
    struct timespec last_statusbar_update = {0};
    (void)clock_gettime(CLOCK_MONOTONIC, &last_statusbar_update);
    struct timespec last_activity = {0};
    (void)clock_gettime(CLOCK_MONOTONIC, &last_activity);
    struct timespec last_prompt_spinner = {0};
    (void)clock_gettime(CLOCK_MONOTONIC, &last_prompt_spinner);
    int bear_ghost_visible = 0;

    /* Agent-quiet mode: when JASH_AGENT is set, suppress the idle statusbar
     * repaint and the prompt-spinner tick. Those emit continuous output while
     * the prompt sits idle (~350 B/s), which makes a quiescence-based terminal
     * reader (e.g. a pty-driven agent harness) hang forever waiting for the
     * stream to settle. A shell that thinks for the LLM is QUIET when idle. */
    int agent_quiet = jash_agent_enabled() || (getenv("JASH_QUIET") != NULL);

    for (;;) {
        char b;
        int rc = r(rctx, &b);
#ifdef JASH_SURFACE_TERMIOS
        /* INV-CONSOLE-004: the backend reports CTRL cancellation separately
         * from an idle timeout. Feed the existing editor's cancel key event. */
        if (rc == 3) { b = 0x03; rc = 1; }
#endif
        
        /* Check if we should update status bar (every 500ms) */
        struct timespec now = {0};
        int clock_ok = clock_gettime(CLOCK_MONOTONIC, &now) == 0;
        long elapsed_ms = clock_ok ?
            (now.tv_sec - last_statusbar_update.tv_sec) * 1000 +
            (now.tv_nsec - last_statusbar_update.tv_nsec) / 1000000 : 0;
        
        if (!agent_quiet && elapsed_ms >= 500) {
            /* Update status bar */
            jash_statusbar_render(w, wctx, s);
            last_statusbar_update = now;
        }

        /* Poll async-signal-safe flags set by handlers */
#ifndef JASH_TARGET_BEYOND
        if (jash_statusbar_sigwinch_pending()) {
            jash_statusbar_clear_sigwinch();
#ifdef JASH_TINALINUX
            if (jash_tina_surface_native_output()) {
                unsigned rows = 0;
                if (jash_tina_surface_viewport(NULL, &rows) == 0 &&
                    jash_statusbar_region_active() && rows >= 5) {
                    char buf[64];
                    int n = snprintf(buf, sizeof(buf), "\033[2;%ur", rows);
                    WRITE_IGNORE(STDOUT_FILENO, buf, n);
                }
            } else
#endif
            {
            struct winsize ws;
            if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0) {
                if (jash_statusbar_region_active() && ws.ws_row >= 5) {
                    char buf[64];
                    int n = snprintf(buf, sizeof(buf), "\033[2;%dr", ws.ws_row);
                    WRITE_IGNORE(STDOUT_FILENO, buf, n);
                }
            }
            }
            jash_statusbar_mark_dirty();
        }
#endif
        if (jash_statusbar_sigterm_pending()) {
            jash_statusbar_clear_sigterm();
            jash_statusbar_teardown_terminal();
            disable_raw_mode();
            return -1;
        }
        
        if (rc == 0)  { disable_raw_mode(); return -1; }  /* EOF */
        if (rc < 0)   { disable_raw_mode(); return -2; }  /* Error */
        if (rc == 2) {
            long spinner_ms = clock_ok ?
                (now.tv_sec - last_prompt_spinner.tv_sec) * 1000 +
                (now.tv_nsec - last_prompt_spinner.tv_nsec) / 1000000 : 0;
            int spinner_context = (L.len == 0 && L.cur == 0) || jash_complete_pager_active();
            if (!agent_quiet && !jash_ai_active() && spinner_ms >= 250 && spinner_context &&
                jash_prompt_spinner_enabled(s)) {
                jash_prompt_spinner_advance(s);
                /* Batch this whole redraw into one write() — see
                 * render_flush_writer's comment above. This tick fires
                 * every 250ms purely to advance the spinner glyph, so an
                 * unbuffered multi-syscall redraw here is the most visible
                 * flicker source of the bunch. */
                render_flush_begin(w, wctx);
                jash_statusbar_render(render_flush_writer, wctx, s);
                jash_prompt_set_compact_redraw(1);
                jash_render_line(render_flush_writer, wctx, s, &L);
                jash_prompt_set_compact_redraw(0);
                render_flush_end();
                last_prompt_spinner = now;
            }

            long idle_ms = (now.tv_sec - last_activity.tv_sec) * 1000 +
                            (now.tv_nsec - last_activity.tv_nsec) / 1000000;
            if (!agent_quiet && !bear_ghost_visible && L.len == 0 && L.cur == 0 &&
                idle_ms >= jash_bear_egg_idle_ms(s)) {
                jash_bear_egg_render_ghost(s, w, wctx);
                render_prompt(s, w, wctx, &L);
                bear_ghost_visible = 1;
            }
            continue;
        }

        int key = jash_keydec_feed(&dec, (uint8_t)b);
        if (key == 0)        continue;
        if (key == JK_UNKNOWN) continue;
        last_activity = now;

        /* INV-PERF10-JASH-PASTE-001: bracketed paste is one literal editor
         * transaction. Do not run auto-pair/abbr/suggestion or publish a
         * complete prompt frame for every byte. */
        if (key == JK_PASTE_START) {
            if (!paste_mode) {
                paste_mode = 1;
                paste_changed = 0;
                tab_after_suggestion = 0;
                jash_complete_reset_cycle();
            }
            continue;
        }
        if (paste_mode) {
            if (key == JK_PASTE_END) {
                paste_mode = 0;
                if (paste_changed) render_prompt(s, w, wctx, &L);
                continue;
            }
            if (key == JK_ENTER)
                paste_changed |= jash_line_insert_byte(&L, '\n');
            else if (key == JK_TAB)
                paste_changed |= jash_line_insert_byte(&L, '\t');
            else if (key >= 0x20 && key < 0x100 && key != 0x7f)
                paste_changed |= jash_line_insert_byte(&L, (char)key);
            continue;
        }
        if (key == JK_PASTE_END) continue;

        /* Vi mode: ESC in insert mode → enter command mode (block cursor) */
        if (s->shell_opts.vi && !vi_cmd_mode && key == JK_ESC) {
            bear_ghost_visible = 0;
            vi_cmd_mode = 1;
            vi_pending  = 0;
            if (L.cur > 0) jash_line_left(&L);
            jash_writes(w, wctx, "\033[2 q");
            render_prompt(s, w, wctx, &L);
            continue;
        }
        /* Vi mode: command-mode key dispatch */
        if (s->shell_opts.vi && vi_cmd_mode && key != JK_ENTER && key != JK_CTRL_C && key != JK_CTRL_D) {
            int changed = 0;
            if (vi_pending == 'r') {
                vi_pending = 0;
                if (key >= 0x20 && key < 0x7F)
                    changed = jash_line_vi_replace_char(&L, (char)key);
            } else if (vi_pending == 'd') {
                vi_pending = 0;
                if (key == 'd') changed = jash_line_clear(&L);
            } else {
                switch (key) {
                case 'h': case JK_LEFT:  changed = jash_line_left(&L);          break;
                case 'l': case JK_RIGHT: changed = jash_line_right(&L);         break;
                case '0': case JK_HOME:  changed = jash_line_home(&L);          break;
                case '$': case JK_END:   changed = jash_line_end(&L);           break;
                case 'w': changed = jash_line_vi_word_fwd(&L);                  break;
                case 'b': changed = jash_line_vi_word_back(&L);                 break;
                case 'e': changed = jash_line_vi_word_end(&L);                  break;
                case 'x': changed = jash_line_delete(&L);                       break;
                case 'X': changed = jash_line_backspace(&L);                    break;
                case 'D': changed = jash_line_kill_eol(&L);                     break;
                case 'p': changed = jash_line_yank(&L);                         break;
                case 'd': vi_pending = 'd';                                      break;
                case 'r': vi_pending = 'r';                                      break;
                case 'i':
                    vi_cmd_mode = 0;
                    jash_writes(w, wctx, "\033[6 q");
                    break;
                case 'I':
                    changed = jash_line_home(&L);
                    vi_cmd_mode = 0;
                    jash_writes(w, wctx, "\033[6 q");
                    break;
                case 'a':
                    jash_line_right(&L);
                    vi_cmd_mode = 0;
                    jash_writes(w, wctx, "\033[6 q");
                    changed = 1;
                    break;
                case 'A':
                    jash_line_end(&L);
                    vi_cmd_mode = 0;
                    jash_writes(w, wctx, "\033[6 q");
                    changed = 1;
                    break;
                case 'C':
                    jash_line_kill_eol(&L);
                    vi_cmd_mode = 0;
                    jash_writes(w, wctx, "\033[6 q");
                    changed = 1;
                    break;
                case 'k': case JK_UP: {
                    uint32_t total = jash_history_count(s);
                    if (total > 0) {
                        if (!hn.active) { hn_save_draft(&hn, &L); hn.active = 1; }
                        if (hn.hist_idx > 0) hn.hist_idx--;
                        hn_load_to_line(s, &hn, &L);
                        changed = 1;
                    }
                    break;
                }
                case 'j': case JK_DOWN: {
                    uint32_t total = jash_history_count(s);
                    if (hn.active) {
                        if (hn.hist_idx < total) hn.hist_idx++;
                        hn_load_to_line(s, &hn, &L);
                        if (hn.hist_idx >= total) hn.active = 0;
                        changed = 1;
                    }
                    break;
                }
                case JK_ESC: vi_pending = 0; break;
                default: break;
                }
            }
            if (changed) render_prompt(s, w, wctx, &L);
            continue;
        }
        /* Vi command mode + Enter/Ctrl+C/Ctrl+D: restore insert mode then fall through */
        if (s->shell_opts.vi && vi_cmd_mode) {
            vi_cmd_mode = 0;
            vi_pending  = 0;
            jash_writes(w, wctx, "\033[6 q");
        }

        if (bear_ghost_visible) {
            bear_ghost_visible = 0;
            if (key == ' ') {
                jash_bear_egg_run(s, r, rctx, w, wctx);
                clock_gettime(CLOCK_MONOTONIC, &last_activity);
                render_prompt(s, w, wctx, &L);
                continue;
            }
            if (key == JK_ENTER || key == JK_ESC) {
                render_prompt(s, w, wctx, &L);
                continue;
            }
            render_prompt(s, w, wctx, &L);
        }

        /* Dispatch. */
        if (jash_complete_pager_key(s, w, wctx, &L, key)) {
            tab_after_suggestion = 0;
            continue;
        }

        if (key == JK_ENTER) {
            /* AI mode: process lines starting with "?" */
            if (L.len > 0 && L.buf[0] == '?') {
                /* Unified AI router (same path as non-interactive -c): `? on`
                 * lazy-loads ai.modjash, `? off` unloads it, `? <query>`
                 * dispatches to the module, `? help/status/qa/...` inline.
                 * L.buf is NUL-terminated at L.len by the line editor. */
                jash_render_newline(w, wctx);
                jash_history_push(s, L.buf);
                jash_ai_route(s, w, wctx, L.buf);
                clock_gettime(CLOCK_MONOTONIC, &last_activity);
                bear_ghost_visible = 0;
                jash_statusbar_mark_dirty();
                render_ai_mode_indicator();

                /* Clear line and re-render prompt */
                jash_line_reset(&L);
                hn.active = 0;
                hn.draft_len = 0;
                hn.hist_idx = jash_history_count(s);
                g_first_render = 1;
                render_prompt(s, w, wctx, &L);
                continue;
            }
            
            jash_render_newline(w, wctx);

            /* BUG-002: agent/session mode (JASH_AGENT/JASH_QUIET) NEVER enters
             * multi-line continuation. A pty-driven agent sends one command per
             * turn and waits for its result; if jash blocked on a `> ` prompt
             * (unbalanced quote from $0 / bracketed paste) the agent's NEXT
             * command got swallowed as continuation → permanent desync / wedge.
             * Evaluate the raw line as-is (the parser is lenient). */
            if (agent_quiet) {
                copy_out(&L, out, out_cap);
                disable_raw_mode();
                return 0;
            }

            /* BUG-008: real multi-line accumulation for humans. The previous
             * code showed a `> ` prompt but then jash_line_reset()'d the buffer,
             * silently DISCARDING the partial line — a quoted/backslash
             * multi-line command lost its first line. Accumulate instead:
             *   - trailing backslash  → join the next line directly (no \n)
             *   - unclosed quote      → the newline is part of the string
             * and only run the command once it parses complete. */
            int trailing_bs = (L.len > 0 && L.buf[L.len-1] == '\\');
            int incomplete = 0;
            if (trailing_bs) {
                accum_append(L.buf, (int)L.len - 1);   /* drop the backslash */
                incomplete = 1;
            } else {
                accum_append(L.buf, (int)L.len);
                if (quotes_open(accum, accum_len)) {
                    accum_append("\n", 1);
                    incomplete = 1;
                }
            }
            /* Overflow guard: stop accumulating and run what we have. */
            if (accum_len >= (int)sizeof(accum) - 2) incomplete = 0;

            if (incomplete) {
                jash_writes(w, wctx, "\r\033[K\033[1;33m> \033[0m");
                jash_line_reset(&L);
                jash_keydec_init(&dec);
                hn.active = 0;
                hn.hist_idx = jash_history_count(s);
                continue;
            }

            /* Complete: the full (possibly multi-line) command is in accum. */
            {
                uint32_t cn = (accum_len < (int)out_cap - 1)
                                ? (uint32_t)accum_len : out_cap - 1;
                for (uint32_t i = 0; i < cn; i++) out[i] = accum[i];
                out[cn] = '\0';
            }
            reset_accum();
            disable_raw_mode();
            return 0;
        }
        if (key == JK_CTRL_C) {
            jash_render_newline(w, wctx);
            reset_accum();            /* drop any pending multi-line buffer */
            jash_line_reset(&L);
            hn.active = 0;
            hn.hist_idx = jash_history_count(s);
            render_prompt(s, w, wctx, &L);
            continue;
        }
        if (key == JK_CTRL_D) {
            if (L.len == 0) { disable_raw_mode(); return -1; }
            jash_line_delete(&L);
            render_prompt(s, w, wctx, &L);
            continue;
        }
        if (key == JK_CTRL_L) {
            jash_writes(w, wctx, "\033[2J\033[H");
            render_prompt(s, w, wctx, &L);
            continue;
        }
        /* Ctrl+P: Command Palette */
        if (key == JK_CTRL_P) {
            int sel = jash_palette_open(s, r, rctx, w, wctx, &L);
            if (sel) {
                /* User selected a command — execute it */
                jash_render_newline(w, wctx);
                copy_out(&L, out, out_cap);
                disable_raw_mode();
                return 0;
            }
            render_prompt(s, w, wctx, &L);
            continue;
        }
        if (key == JK_CTRL_R) {
            char search[64]; int slen = 0;
            const char* match_line = 0;
            static int fuzzy_prev_idx = -1;
            jash_writes(w, wctx, "\r\033[K\033[36m(reverse-i-search)`'\033[0m");
            for (;;) {
                char sb;
                if (r(rctx, &sb) <= 0) break;
                int sk = jash_keydec_feed(&dec, (uint8_t)sb);
                if (sk == JK_CTRL_R) {
                    if (slen > 0 && match_line) {
                        match_line = jash_jux_fuzzy_history(s, search, fuzzy_prev_idx);
                        if (!match_line) {
                            fuzzy_prev_idx = -1;
                            match_line = jash_jux_fuzzy_history(s, search, -1);
                        }
                    }
                } else if (sk == JK_ENTER || sk == JK_CTRL_C || sk == JK_ESC) {
                    if (sk == JK_ESC || sk == JK_CTRL_C) { match_line = 0; break; }
                    break;
                } else if (sk == JK_BACKSPACE || sk == 0x7F) {
                    if (slen > 0) { slen--; search[slen] = 0; match_line = 0; fuzzy_prev_idx = -1; }
                } else if (sk >= 0x20 && sk < 0x7F && slen < 62) {
                    search[slen++] = (char)sk; search[slen] = 0; match_line = 0; fuzzy_prev_idx = -1;
                }
                if (!match_line && slen > 0) {
                    match_line = jash_jux_fuzzy_history(s, search, fuzzy_prev_idx);
                    if (match_line) {
                        uint32_t hc = jash_history_count(s);
                        for (uint32_t fi = 0; fi < hc; fi++) {
                            if (jash_history_at(s, fi) == match_line) {
                                fuzzy_prev_idx = (int)fi;
                                break;
                            }
                        }
                    }
                }
                jash_writes(w, wctx, "\r\033[K\033[36m(reverse-i-search)`");
                jash_writes_sanitized(w, wctx, search);
                jash_writes(w, wctx, "'\033[0m");
                if (match_line) {
                    jash_writes(w, wctx, ": ");
                    jash_writes_sanitized(w, wctx, match_line);
                }
            }
            jash_writes(w, wctx, "\r\033[K");
            if (match_line && slen > 0) {
                const char *hl = match_line;
                L.len = 0; L.cur = 0;
                while (*hl && L.len < JASH_LINE_MAX) L.buf[L.len++] = *hl++;
                L.cur = L.len;
                fuzzy_prev_idx = -1;
            } else {
                fuzzy_prev_idx = -1;
            }
            render_prompt(s, w, wctx, &L);
            continue;
        }
        if (key == JK_TAB) {
            if (tab_after_suggestion) {
                tab_after_suggestion = 0;
                jash_complete_command_hud(s, w, wctx, &L);
                continue;
            }
            /* File/command completion has priority over history suggestions. */
            if (jash_complete_tab(s, w, wctx, &L)) {
                tab_after_suggestion = 0;
                continue;
            }
            /* Nothing matched — fall back to inline history autosuggest. */
            char sug[JASH_LINE_MAX];
            if (isatty(STDIN_FILENO) && isatty(STDOUT_FILENO) &&
                jash_jux_autosuggest(s, &L, sug, sizeof(sug)) &&
                jash_jux_accept_suggestion(&L, sug)) {
                tab_after_suggestion = 1;
                render_prompt(s, w, wctx, &L);
                continue;
            }
            /* Try smart suggestions (next-command prediction from patterns). */
            if (s->ss_engine) {
                const char* ss_sug = jash_ss_get_suggestion(s->ss_engine, L.buf);
                if (ss_sug && ss_sug[0]) {
                    char full[JASH_LINE_MAX];
                    int n = snprintf(full, sizeof(full), " %s", ss_sug);
                    if (n > 0 && (size_t)n < sizeof(full) &&
                        jash_jux_accept_suggestion(&L, full)) {
                        tab_after_suggestion = 1;
                        render_prompt(s, w, wctx, &L);
                        continue;
                    }
                }
            }
            /* No candidates and no suggestion — do nothing. */
            continue;
        }

        if (key == JK_UP || key == JK_CTRL_P) {
            uint32_t total = jash_history_count(s);
            if (total == 0) continue;
            if (!hn.active) { hn_save_draft(&hn, &L); hn.active = 1; }
            if (hn.hist_idx > 0) hn.hist_idx--;
            hn_load_to_line(s, &hn, &L);
            render_prompt(s, w, wctx, &L);
            continue;
        }
        if (key == JK_DOWN || key == JK_CTRL_N) {
            uint32_t total = jash_history_count(s);
            if (!hn.active) continue;
            if (hn.hist_idx < total) hn.hist_idx++;
            hn_load_to_line(s, &hn, &L);
            if (hn.hist_idx >= total) hn.active = 0;
            render_prompt(s, w, wctx, &L);
            continue;
        }

        if (key == JK_RIGHT || key == JK_CTRL_F) {
            char sug[JASH_LINE_MAX];
            if (jash_jux_autosuggest(s, &L, sug, sizeof(sug)) &&
                jash_jux_accept_suggestion(&L, sug)) {
                render_prompt(s, w, wctx, &L);
                continue;
            }
        }

        /* Auto-pair: handle printable characters through autopair first */
        if (key >= 0x20 && key < 0x7F) {
            tab_after_suggestion = 0;
            jash_complete_reset_cycle();
            int ap = jash_autopair_feed(&L, (char)key);
            if (ap == 0) {
                /* No autopair action — normal insert */
                int changed = jash_line_insert_byte(&L, (char)key);
                if (changed) {
                    if (key == ' ') jash_jux_expand_abbr(s, &L);
                    render_prompt(s, w, wctx, &L);
                }
            } else {
                /* Autopair did something — redraw */
                if (key == ' ') jash_jux_expand_abbr(s, &L);
                render_prompt(s, w, wctx, &L);
            }
            continue;
        }

        /* Other editing keys */
        int changed = 0;
        switch (key) {
        case JK_LEFT:   case JK_CTRL_B: tab_after_suggestion = 0; jash_complete_reset_cycle(); changed = jash_line_left(&L);      break;
        case JK_RIGHT:  case JK_CTRL_F: tab_after_suggestion = 0; jash_complete_reset_cycle(); changed = jash_line_right(&L);     break;
        case JK_HOME:   case JK_CTRL_A: tab_after_suggestion = 0; jash_complete_reset_cycle(); changed = jash_line_home(&L);      break;
        case JK_END:    case JK_CTRL_E: tab_after_suggestion = 0; jash_complete_reset_cycle(); changed = jash_line_end(&L);       break;
        case JK_BACKSPACE:
            tab_after_suggestion = 0;
            jash_complete_reset_cycle();
            /* Auto-pair: delete matching pair if between brackets */
            changed = jash_autopair_backspace(&L);
            if (!changed) changed = jash_line_backspace(&L);
            break;
        case JK_DELETE:  tab_after_suggestion = 0; jash_complete_reset_cycle(); changed = jash_line_delete(&L);    break;
        case JK_CTRL_K:  tab_after_suggestion = 0; changed = jash_line_kill_eol(&L);  break;
        case JK_CTRL_U:  tab_after_suggestion = 0; changed = jash_line_kill_bol(&L);  break;
        case JK_CTRL_W:  tab_after_suggestion = 0; changed = jash_line_kill_word(&L); break;
        case JK_CTRL_Y:  tab_after_suggestion = 0; changed = jash_line_yank(&L);      break;
        default:
            break;
        }
        if (changed) render_prompt(s, w, wctx, &L);
    }
}
