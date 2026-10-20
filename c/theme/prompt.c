// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — theme/prompt — intelligent prompt with git status, duration, exit code.

#include "prompt.h"
#include "colors.h"
#include "io.h"
#include "theme.h"
#include <core/env.h>
#include <core/jash_ai.h>
#include <ui/statusbar.h>
#include <sys/syscalls.h>
#include <sys/ioctl.h>
#include <string.h>
#include <stdlib.h>

const char* const JASH_HOSTNAME = "fermihart";
/* INV-889: global no-color flag — set by jash_theme_init when NO_COLOR
 * or TERM=dumb is active. Every hardcoded ANSI token in the prompt goes
 * through pc() which returns "" when colors are off. */
int g_jash_no_color = 0;
static const char* pc(const char* ansi) { return g_jash_no_color ? "" : ansi; }

static unsigned g_prompt_spinner_frame = 0;
static int g_prompt_compact_redraw = 0;

/* Git cache: avoid repeated find_git_root sys_open calls on every prompt render.
 * Invalidated when cwd changes. */
static char g_prompt_git_cached_cwd[512] = "";
static char g_prompt_git_cached_branch[64] = "";
static int g_prompt_git_cached_has = 0;
static int g_prompt_git_cached_dirty = 0;

static unsigned str_len(const char* s) {
    unsigned n = 0;
    while (s && s[n]) n++;
    return n;
}

static void copy_str(char* dst, unsigned cap, const char* src) {
    unsigned i = 0;
    if (cap == 0) return;
    while (src && src[i] && i + 1 < cap) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static const char* prompt_env_get(const jash_state_t* s, const char* key) {
    const char* val = jash_env_get(s, key);
    return val ? val : getenv(key);
}

static int ascii_lower(int c) {
    return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

static int contains_ascii_ci(const char* s, const char* needle) {
    if (!s || !needle || !needle[0]) return 0;
    for (unsigned i = 0; s[i]; i++) {
        unsigned j = 0;
        while (needle[j] && s[i + j] && ascii_lower((unsigned char)s[i + j]) == ascii_lower((unsigned char)needle[j])) j++;
        if (!needle[j]) return 1;
    }
    return 0;
}

static int prompt_utf8_ok(const jash_state_t* s) {
    const char* term = prompt_env_get(s, "TERM");
    if (term && strcmp(term, "dumb") == 0) return 0;
    return contains_ascii_ci(prompt_env_get(s, "LC_ALL"), "utf-8") ||
           contains_ascii_ci(prompt_env_get(s, "LC_ALL"), "utf8") ||
           contains_ascii_ci(prompt_env_get(s, "LC_CTYPE"), "utf-8") ||
           contains_ascii_ci(prompt_env_get(s, "LC_CTYPE"), "utf8") ||
           contains_ascii_ci(prompt_env_get(s, "LANG"), "utf-8") ||
           contains_ascii_ci(prompt_env_get(s, "LANG"), "utf8");
}

static const char* prompt_sigil(const jash_state_t* s) {
    static const char* utf8_frames[] = {
        "\xe2\xa0\x8b", "\xe2\xa0\x99", "\xe2\xa0\xb9", "\xe2\xa0\xb8",
        "\xe2\xa0\xbc", "\xe2\xa0\xb4", "\xe2\xa0\xa6", "\xe2\xa0\xa7",
        "\xe2\xa0\x87", "\xe2\xa0\x8f"
    };
    static const char* ascii_frames[] = { "-", "\\", "|", "/" };

    if (jash_prompt_spinner_enabled(s) && g_prompt_compact_redraw) {
        if (prompt_utf8_ok(s)) return utf8_frames[g_prompt_spinner_frame % 10];
        return ascii_frames[g_prompt_spinner_frame % 4];
    }

    const char* sigil = prompt_env_get(s, "JASH_PROMPT_SIGIL");
    if (!sigil) return "$";
    if (strcmp(sigil, "dollar") == 0) return "$";
    if (strcmp(sigil, "bear") == 0) return "(B)";
    if (strcmp(sigil, "paw") == 0 && prompt_utf8_ok(s)) return "\xf0\x9f\x90\xbe";
    return "$";
}

int jash_prompt_spinner_enabled(const jash_state_t* s) {
    const char* val = prompt_env_get(s, "JASH_PROMPT_SPINNER");
    if (!val) return 1;
    if (strcmp(val, "0") == 0 || strcmp(val, "off") == 0 || strcmp(val, "no") == 0)
        return 0;
    return 1;
}

void jash_prompt_spinner_advance(const jash_state_t* s) {
    if (!jash_prompt_spinner_enabled(s)) return;
    g_prompt_spinner_frame++;
}

void jash_prompt_set_compact_redraw(int compact) {
    g_prompt_compact_redraw = compact ? 1 : 0;
}

static int join_git_path(char* out, unsigned cap, const char* dir, const char* leaf) {
    unsigned i = 0;
    if (cap == 0 || !dir || !leaf) return 0;
    while (dir[i] && i + 1 < cap) { out[i] = dir[i]; i++; }
    if (i > 1 && out[i - 1] == '/') i--;
    const char* mid = "/.git/";
    for (unsigned k = 0; mid[k] && i + 1 < cap; k++) out[i++] = mid[k];
    for (unsigned k = 0; leaf[k] && i + 1 < cap; k++) out[i++] = leaf[k];
    out[i] = 0;
    return leaf[0] && leaf[str_len(leaf)] == 0;
}

static int find_git_root(const char* cwd, char* out, unsigned cap) {
    char cur[512];
    copy_str(cur, sizeof(cur), cwd);

    while (cur[0]) {
        char head[640];
        join_git_path(head, sizeof(head), cur, "HEAD");
        long fd = sys_open(head, 0, 0);
        if (fd >= 0) {
            sys_close((int)fd);
            copy_str(out, cap, cur);
            return 1;
        }

        unsigned len = str_len(cur);
        if (len <= 1) break;
        while (len > 1 && cur[len - 1] == '/') len--;
        while (len > 1 && cur[len - 1] != '/') len--;
        if (len <= 1) {
            cur[0] = '/';
            cur[1] = 0;
        } else {
            cur[len - 1] = 0;
        }
    }
    return 0;
}

static int get_git_branch_at(const char* root, char* out, unsigned cap) {
    char head_path[640];
    join_git_path(head_path, sizeof(head_path), root, "HEAD");
    long fd = sys_open(head_path, 0, 0);
    if (fd < 0) return 0;
    char buf[256];
    long n = sys_read((int)fd, buf, sizeof(buf) - 1);
    sys_close((int)fd);
    if (n <= 0) return 0;
    buf[n] = 0;

    if (buf[0] == 'r' && buf[1] == 'e' && buf[2] == 'f' && buf[3] == ':') {
        const char* ref = buf + 5;
        const char* last_slash = ref;
        const char* p = ref;
        while (*p && *p != '\n') {
            if (*p == '/') last_slash = p + 1;
            p++;
        }
        unsigned i = 0;
        while (last_slash[i] && last_slash[i] != '\n' && i < cap - 1) {
            out[i] = last_slash[i];
            i++;
        }
        out[i] = 0;
        return 1;
    }
    unsigned i = 0;
    while (buf[i] && buf[i] != '\n' && i < cap - 1) {
        out[i] = buf[i];
        i++;
    }
    out[i] = 0;
    return 1;
}

static int get_git_dirty_at(const char* root) {
    char index_path[640];
    join_git_path(index_path, sizeof(index_path), root, "index");
    long fd = sys_open(index_path, 0, 0);
    if (fd < 0) return 0;
    struct stat st;
    int r = fstat((int)fd, &st);
    sys_close((int)fd);
    if (r < 0) return 0;

    char head_path[640];
    join_git_path(head_path, sizeof(head_path), root, "HEAD");
    long wfd = sys_open(head_path, 0, 0);
    if (wfd < 0) return 0;
    struct stat wst;
    int wr = fstat((int)wfd, &wst);
    sys_close((int)wfd);
    if (wr < 0) return 0;

    return wst.st_mtime > st.st_mtime;
}

static void format_cmd_duration(char* out, unsigned cap, const jash_state_t* s) {
    if (s->last_status.time_ns == 0 || cap == 0) { out[0] = 0; return; }
    uint64_t ns = s->last_status.time_ns;
    unsigned i = 0;

    if (ns < 1000000ULL) {
        int us = (int)(ns / 1000);
        char tmp[16]; int ti = 0;
        if (us == 0) tmp[ti++] = '0';
        else { while (us > 0 && ti < 15) { tmp[ti++] = '0' + us % 10; us /= 10; } }
        for (int j = ti - 1; j >= 0 && i < cap - 4; j--) out[i++] = tmp[j];
        out[i++] = '\xc2'; out[i++] = '\xb5'; out[i++] = 's';
    } else if (ns < 1000000000ULL) {
        int ms = (int)(ns / 1000000);
        char tmp[16]; int ti = 0;
        if (ms == 0) tmp[ti++] = '0';
        else { while (ms > 0 && ti < 15) { tmp[ti++] = '0' + ms % 10; ms /= 10; } }
        for (int j = ti - 1; j >= 0 && i < cap - 3; j--) out[i++] = tmp[j];
        out[i++] = 'm'; out[i++] = 's';
    } else {
        int sec = (int)(ns / 1000000000ULL);
        char tmp[16]; int ti = 0;
        if (sec == 0) tmp[ti++] = '0';
        else { while (sec > 0 && ti < 15) { tmp[ti++] = '0' + sec % 10; sec /= 10; } }
        for (int j = ti - 1; j >= 0 && i < cap - 2; j--) out[i++] = tmp[j];
        out[i++] = 's';
    }
    out[i] = '\0';
}

#ifdef JASH_TINALINUX
static void prompt_render_lineage(jash_writer_t w, void* ctx, const jash_state_t* s) {
    const char* lin = prompt_env_get(s, "TINA_LINEAGE");
    if (!lin) return;
    for (int i = 0; i < 64; i++) {
        char c = lin[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F'))) return;
    }
    if (lin[64]) return;
    char short_root[9];
    memcpy(short_root, lin, 8);
    short_root[8] = 0;
    jash_writes(w, ctx, pc("\033[90m[g:"));
    jash_writes(w, ctx, short_root);
    const char* epoch = prompt_env_get(s, "TINA_EPOCH");
    if (epoch && *epoch) {
        jash_writes(w, ctx, ":e");
        jash_writes_sanitized(w, ctx, epoch);
    }
    const char* fork_id = prompt_env_get(s, "TINA_FORK");
    if (fork_id && *fork_id) {
        jash_writes(w, ctx, ":f");
        jash_writes_sanitized(w, ctx, fork_id);
    }
    jash_writes(w, ctx, pc("]\033[0m"));
}
#endif

void jash_prompt_render(jash_writer_t w, void* ctx, const jash_state_t* s) {
    if (!w || !s) return;

    const jash_theme_t* theme = jash_theme_current();

    /* AI mode indicator (top-right when statusbar is off).
     * Single implementation lives in ui/statusbar.c — it saves/restores
     * the cursor itself, so the prompt never touches row 1. */
    if (!jash_statusbar_enabled(s) && jash_ai_active())
        render_ai_mode_indicator();

    const char* user = jash_env_get(s, "USER");
    if (!user) user = "fermi";

    /* Git cache: only re-probe when cwd changes */
    int has_git, git_dirty;
    char git_branch[64];
    if (strcmp(g_prompt_git_cached_cwd, s->cwd) != 0) {
        char git_root[512];
        g_prompt_git_cached_has = find_git_root(s->cwd, git_root, sizeof(git_root)) &&
                                  get_git_branch_at(git_root, g_prompt_git_cached_branch, sizeof(g_prompt_git_cached_branch));
        g_prompt_git_cached_dirty = g_prompt_git_cached_has ? get_git_dirty_at(git_root) : 0;
        unsigned i = 0;
        while (s->cwd[i] && i + 1 < sizeof(g_prompt_git_cached_cwd)) {
            g_prompt_git_cached_cwd[i] = s->cwd[i];
            i++;
        }
        g_prompt_git_cached_cwd[i] = '\0';
    }
    has_git = g_prompt_git_cached_has;
    git_dirty = g_prompt_git_cached_dirty;
    unsigned bi = 0;
    while (g_prompt_git_cached_branch[bi] && bi + 1 < sizeof(git_branch)) {
        git_branch[bi] = g_prompt_git_cached_branch[bi];
        bi++;
    }
    git_branch[bi] = '\0';
    int git_in_statusbar = jash_statusbar_enabled(s);

    char duration[32];
    format_cmd_duration(duration, sizeof(duration), s);

    /* Keep the main prompt short. If the statusbar is off, show git context in
     * a compact line above the prompt instead of widening the input line. */
    if (has_git && !git_in_statusbar && !g_prompt_compact_redraw &&
        !jash_prompt_spinner_enabled(s)) {
        jash_writes(w, ctx, pc("\033[90mgit:"));
        jash_writes(w, ctx, theme->role_accent);
        jash_writes_sanitized(w, ctx, git_branch);
        jash_writes(w, ctx, "\033[0m");
        if (git_dirty) jash_writes(w, ctx, pc("\033[91m*\033[0m"));
        jash_writes(w, ctx, "\r\n");
    }

    /* User */
    if (jash_ai_active()) {
        /* Compact AI prompt: ai ~path ? */
        jash_writes(w, ctx, pc("\033[1;36mai\033[0m"));
        jash_writes(w, ctx, theme->role_path);
        {
            const char *home = getenv("HOME");
            if (home && strncmp(s->cwd, home, strlen(home)) == 0) {
                jash_writes(w, ctx, "~");
                jash_writes_sanitized(w, ctx, s->cwd + strlen(home));
            } else {
                jash_writes_sanitized(w, ctx, s->cwd);
            }
        }
        jash_writes(w, ctx, "\033[0m");
        jash_writes(w, ctx, " ");
        jash_writes(w, ctx, theme->role_sigil);
        jash_writes(w, ctx, pc("\033[1;36m"));
        jash_writes(w, ctx, "?");
        jash_writes(w, ctx, "\033[0m ");
        return;
    }

    /* Normal prompt: user@hostname:path$ */
    jash_writes(w, ctx, theme->role_user);
    jash_writes_sanitized(w, ctx, user);
    jash_writes(w, ctx, "\033[0m");

    /* @ */
    jash_writes(w, ctx, theme->role_accent);
    jash_writes(w, ctx, "@");
    jash_writes(w, ctx, "\033[0m");

    /* Host */
    jash_writes(w, ctx, theme->role_host);
    jash_writes(w, ctx, JASH_HOSTNAME);
    jash_writes(w, ctx, "\033[0m");
#ifdef JASH_TINALINUX
    prompt_render_lineage(w, ctx, s);
#endif

    jash_writes(w, ctx, ":");

    /* CWD — abbreviate home dir as ~ */
    jash_writes(w, ctx, theme->role_path);
    {
        const char *home = getenv("HOME");
        if (home && strncmp(s->cwd, home, strlen(home)) == 0) {
            jash_writes(w, ctx, "~");
            jash_writes_sanitized(w, ctx, s->cwd + strlen(home));
        } else {
            jash_writes_sanitized(w, ctx, s->cwd);
        }
    }
    jash_writes(w, ctx, "\033[0m");

    /* Exit status */
    if (s->last_status.code != 0) {
        jash_writes(w, ctx, theme->role_err);
        jash_writef(w, ctx, "[%d]", s->last_status.code);
        jash_writes(w, ctx, "\033[0m");
    }

    /* Duration (color-coded: gray <1s, yellow 1-5s, red >5s) */
    if (duration[0]) {
        uint64_t ns = s->last_status.time_ns;
        const char *color = "\033[90m";  /* gray for <1s */
        if (ns >= 5000000000ULL)       color = "\033[1;31m"; /* red bold for >5s */
        else if (ns >= 1000000000ULL)  color = "\033[1;33m"; /* yellow bold for 1-5s */
        jash_writes(w, ctx, " ");
        jash_writes(w, ctx, color);
        jash_writes(w, ctx, duration);
        jash_writes(w, ctx, "\033[0m");
    }

    /* Sigil */
    jash_writes(w, ctx, " ");
    jash_writes(w, ctx, theme->role_sigil);
    jash_writes(w, ctx, pc("\033[1m"));
    jash_writes(w, ctx, prompt_sigil(s));
    jash_writes(w, ctx, "\033[0m ");
}
