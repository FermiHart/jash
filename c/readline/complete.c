// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — complete — TAB completion with cycling support.
//
// Strategy:
//   1. First token → command completion (builtins + PATH)
//   2. Second token → contextual (git/npm/cargo/docker subcommands, theme names)
//   3. Otherwise → file completion
//   4. Unique match → finish + space. Multiple → cycle through on repeated TAB.
//
// Cycling: when there are multiple candidates and the LCP doesn't grow,
// repeated TAB presses cycle through them one by one (like Fish/zsh).

#include "complete.h"
#include "edit.h"
#include "render.h"
#include "keymap.h"
#include <builtins/builtin.h>
#include <core/state.h>
#include <core/env.h>
#include <sys/syscalls.h>
#include <theme/theme.h>
#include "osif/osinfo.h"
#include <dirent.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#ifdef JASH_TINALINUX
#include <osif/tina/surface.h>
#endif
#include <unistd.h>
#include <stdlib.h>
#include <time.h>

#ifndef DT_DIR
#define DT_DIR 4
#endif

static uint32_t my_slen(const char* s) {
    uint32_t n = 0; while (s[n]) n++; return n;
}

static uint32_t token_start(const jash_line_t* L) {
    uint32_t i = L->cur;
    while (i > 0 && L->buf[i - 1] != ' ' && L->buf[i - 1] != '\t') i--;
    return i;
}

static int is_first_token(const jash_line_t* L, uint32_t tstart) {
    for (uint32_t i = 0; i < tstart; i++) {
        if (L->buf[i] != ' ' && L->buf[i] != '\t') return 0;
    }
    return 1;
}

static int my_strncmp(const char* a, const char* b, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
        if (!a[i]) return 0;
    }
    return 0;
}

static int my_streq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

static void first_token_name(const jash_line_t* L, char* out, uint32_t cap) {
    uint32_t i = 0, k = 0;
    while (i < L->len && (L->buf[i] == ' ' || L->buf[i] == '\t')) i++;
    while (i < L->len && L->buf[i] != ' ' && L->buf[i] != '\t' && k + 1 < cap)
        out[k++] = L->buf[i++];
    out[k] = 0;
}

/* ---- candidate storage ---- */
#define COMP_MAX_CANDS  64
#define COMP_MAX_NAME   64

typedef struct {
    char    name[COMP_MAX_NAME];
    int     is_dir;
} cand_t;

/* ---- flag help cache ---- */
#define FLAG_CACHE_SIZE 8
#define FLAG_NEG_TTL_SEC 300  /* re-probe a failed --help after 5 minutes */
typedef struct {
    char    cmd[COMP_MAX_NAME];
    char    flags[1024];
    time_t  stamp;   /* negative entries: when the probe failed */
} flag_cache_t;
static flag_cache_t flag_cache[FLAG_CACHE_SIZE];
static int flag_cache_count = 0;

/* Check flag cache for a command. Returns flags string or NULL.
 * A negative hit (empty flags: the probe failed or yielded no flags)
 * suppresses re-forking on every TAB, but expires so a command that
 * was installed/fixed later gets one fresh probe. */
static const char* flag_cache_lookup(const char* cmd) {
    for (int i = 0; i < flag_cache_count; i++) {
        if (my_streq(flag_cache[i].cmd, cmd)) {
            if (flag_cache[i].flags[0] == '\0' &&
                time(NULL) - flag_cache[i].stamp > FLAG_NEG_TTL_SEC) {
                flag_cache[i] = flag_cache[--flag_cache_count];  /* evict */
                return NULL;
            }
            return flag_cache[i].flags;
        }
    }
    return NULL;
}

/* Store flags in cache for a command (empty flags = negative entry). */
static void flag_cache_store(const char* cmd, const char* flags) {
    if (flag_cache_count < FLAG_CACHE_SIZE) {
        uint32_t cl = my_slen(cmd);
        if (cl >= COMP_MAX_NAME) cl = COMP_MAX_NAME - 1;
        for (uint32_t i = 0; i < cl; i++) flag_cache[flag_cache_count].cmd[i] = cmd[i];
        flag_cache[flag_cache_count].cmd[cl] = '\0';
        uint32_t fl = my_slen(flags);
        if (fl >= sizeof(flag_cache[0].flags)) fl = sizeof(flag_cache[0].flags) - 1;
        for (uint32_t i = 0; i < fl; i++) flag_cache[flag_cache_count].flags[i] = flags[i];
        flag_cache[flag_cache_count].flags[fl] = '\0';
        flag_cache[flag_cache_count].stamp = time(NULL);
        flag_cache_count++;
    }
}

/* Pad with one write per run of spaces instead of one write(2) per space. */
static void write_pad(jash_writer_t w, void* wctx, int n) {
    char buf[65];
    while (n > 0) {
        int chunk = n > 64 ? 64 : n;
        for (int i = 0; i < chunk; i++) buf[i] = ' ';
        buf[chunk] = '\0';
        jash_writes(w, wctx, buf);
        n -= chunk;
    }
}

static int has_suffix(const char* s, const char* suffix) {
    uint32_t sl = my_slen(s);
    uint32_t xl = my_slen(suffix);
    if (xl > sl) return 0;
    return my_streq(s + sl - xl, suffix);
}

static int is_binaryish_name(const char* name) {
    static const char* blocked[] = {
        ".o", ".a", ".so", ".dylib", ".dll", ".exe", ".bin",
        ".tar", ".gz", ".tgz", ".zip", ".7z", ".rar",
        ".png", ".jpg", ".jpeg", ".gif", ".webp", ".pdf",
        ".dSYM", NULL
    };
    for (int i = 0; blocked[i]; i++) {
        if (has_suffix(name, blocked[i])) return 1;
    }
    return 0;
}

static int is_textish_name(const char* name) {
    static const char* allowed[] = {
        ".txt", ".md", ".c", ".h", ".cc", ".cpp", ".hpp",
        ".asm", ".s", ".sh", ".json", ".xml", ".log", ".toml",
        ".yaml", ".yml", ".ini", ".conf", ".rs", ".py", ".js",
        ".ts", ".css", ".html", NULL
    };
    if (is_binaryish_name(name)) return 0;
    for (int i = 0; allowed[i]; i++) {
        if (has_suffix(name, allowed[i])) return 1;
    }
    return 0;
}

static int is_search_ignored_name(const char* name) {
    static const char* ignored[] = {
        "build", "target", "node_modules", ".git", ".hg", ".svn",
        ".cache", "__pycache__", ".pytest_cache", ".mypy_cache", NULL
    };
    for (int i = 0; ignored[i]; i++) {
        if (my_streq(name, ignored[i])) return 1;
    }
    return 0;
}

static int is_reader_cmd(const char* cmd) {
    return my_streq(cmd, "cat") || my_streq(cmd, "less") || my_streq(cmd, "more") ||
           my_streq(cmd, "head") || my_streq(cmd, "tail") || my_streq(cmd, "wc");
}

static int is_passthrough_cmd(const char* cmd) {
    return my_streq(cmd, "sudo") || my_streq(cmd, "doas") ||
           my_streq(cmd, "env") || my_streq(cmd, "nohup") ||
           my_streq(cmd, "time") || my_streq(cmd, "command") ||
           my_streq(cmd, "ltrace") || my_streq(cmd, "strace") ||
           my_streq(cmd, "nice") || my_streq(cmd, "chrt");
}

static int is_io_path_cmd(const char* cmd) {
    return my_streq(cmd, "ls") || is_reader_cmd(cmd) || my_streq(cmd, "grep") ||
           my_streq(cmd, "tar");
}

static int path_is_dir(const char* path) {
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int path_is_regular(const char* path) {
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static void cand_add(cand_t* cs, int* nc, const char* name, int is_dir) {
    if (*nc >= COMP_MAX_CANDS) return;
    for (int i = 0; i < *nc; i++) {
        if (my_streq(cs[i].name, name)) return;
    }
    cand_t* c = &cs[*nc];
    uint32_t i = 0;
    while (name[i] && i + 1 < COMP_MAX_NAME) { c->name[i] = name[i]; i++; }
    c->name[i] = '\0';
    c->is_dir = is_dir;
    (*nc)++;
}

/* ---- builtin enumeration ---- */
static void collect_builtins(const char* prefix, uint32_t plen,
                             cand_t* cs, int* nc) {
    uint32_t total = jash_builtin_count();
    for (uint32_t i = 0; i < total; i++) {
        const jash_builtin_t* b = jash_builtin_at(i);
        if (!b || !b->name) continue;
        if (plen == 0 || my_strncmp(b->name, prefix, plen) == 0) {
            cand_add(cs, nc, b->name, 0);
        }
    }
}

static void collect_words(const char* const* words, const char* prefix,
                          uint32_t plen, cand_t* cs, int* nc) {
    for (int i = 0; words[i]; i++) {
        if (plen == 0 || my_strncmp(words[i], prefix, plen) == 0)
            cand_add(cs, nc, words[i], 0);
    }
}

/* ---- collect theme names as candidates ---- */
static void collect_theme_names(const char* prefix, uint32_t plen,
                                cand_t* cs, int* nc) {
    static const char* themes[] = {
        "midnight", "synthwave", "amber", "1990",
        "dracula", "nord", "solarized", "monokai",
        "--list", "--current", "--help",
        NULL
    };
    collect_words(themes, prefix, plen, cs, nc);
}

static void collect_git_state_aware(const char* prefix, uint32_t plen,
                                    cand_t* cs, int* nc) {
    /* Suggest git subcommands first */
    static const char* git_cmds[] = {
        "add", "branch", "checkout", "clone", "commit", "diff", "fetch",
        "log", "merge", "pull", "push", "rebase", "status", "stash",
        "switch", "restore", "tag", "reset", "revert", "--help", NULL
    };
    collect_words(git_cmds, prefix, plen, cs, nc);

    /* Collect local branches from .git/refs/heads/ */
    DIR* dir = opendir(".git/refs/heads");
    if (dir) {
        struct dirent* entry;
        while ((entry = readdir(dir)) != NULL) {
            const char* nm = entry->d_name;
            if (nm[0] == '.' && (nm[1] == '\0' || (nm[1] == '.' && nm[2] == '\0')))
                continue;
            if (plen == 0 || my_strncmp(nm, prefix, plen) == 0) {
                cand_add(cs, nc, nm, 0);
            }
        }
        closedir(dir);
    }
}

typedef struct { const char* prefix; uint32_t plen; cand_t* cs; int* nc; } proc_cand_ctx_t;

static int proc_cand_cb(const osif_procinfo_t* info, void* user) {
    proc_cand_ctx_t* c = (proc_cand_ctx_t*)user;
    if (!info->comm[0]) return 0;
    if (c->plen == 0 || my_strncmp(info->comm, c->prefix, c->plen) == 0)
        cand_add(c->cs, c->nc, info->comm, 0);
    return 0;
}

static void collect_proc_names(const char* prefix, uint32_t plen,
                               cand_t* cs, int* nc) {
    proc_cand_ctx_t ctx = { prefix, plen, cs, nc };
    bear_osinfo_proc_iter(proc_cand_cb, &ctx);
}

static void collect_ssh_hosts(jash_state_t* s, const char* prefix,
                               uint32_t plen, cand_t* cs, int* nc) {
    char buf[16384];
    const char* home = jash_env_get(s, "HOME");
    if (!home) home = "/root";
    char kpath[256];
    uint32_t kk = 0;
    for (const char* h = home; *h && kk + 1 < sizeof(kpath); h++) kpath[kk++] = *h;
    const char* suf = "/.ssh/known_hosts";
    for (const char* h = suf; *h && kk + 1 < sizeof(kpath); h++) kpath[kk++] = *h;
    kpath[kk] = '\0';
    long fd = sys_open(kpath, 0, 0);
    if (fd >= 0) {
        long nr = sys_read((int)fd, buf, sizeof(buf) - 1);
        sys_close((int)fd);
        if (nr > 0) {
            buf[nr] = '\0';
            const char* q = buf;
            while (*q) {
                while (*q && *q != ' ' && *q != '\n') q++;
                if (*q == '\n') { q++; continue; }
                if (*q == ' ') q++;
                while (*q && *q != ' ' && *q != '\n') {
                    const char* hn = q;
                    while (*q && *q != ' ' && *q != '\n' && *q != ',') q++;
                    uint32_t hnlen = (uint32_t)(q - hn);
                    if (hnlen > 0 && hnlen < COMP_MAX_NAME) {
                        char hnbuf[COMP_MAX_NAME];
                        for (uint32_t hni = 0; hni < hnlen; hni++) hnbuf[hni] = hn[hni];
                        hnbuf[hnlen] = '\0';
                        int is_ip = 1;
                        for (uint32_t hni = 0; hni < hnlen; hni++)
                            if (!((hnbuf[hni] >= '0' && hnbuf[hni] <= '9') || hnbuf[hni] == '.' || hnbuf[hni] == ':'))
                                { is_ip = 0; break; }
                        if (!is_ip && (plen == 0 || my_strncmp(hnbuf, prefix, plen) == 0))
                            cand_add(cs, nc, hnbuf, 0);
                    }
                    if (*q == ',') q++;
                }
                while (*q && *q != '\n') q++;
                if (*q == '\n') q++;
            }
        }
    }
}

static void collect_make_targets(const char* prefix, uint32_t plen,
                                  cand_t* cs, int* nc) {
    /* Try Makefile, makefile, GNUmakefile */
    const char* mfiles[] = {"Makefile", "makefile", "GNUmakefile", 0};
    for (int mi = 0; mfiles[mi]; mi++) {
        char buf[8192];
        long fd = sys_open(mfiles[mi], 0, 0);
        if (fd < 0) continue;
        long nr = sys_read((int)fd, buf, sizeof(buf) - 1);
        sys_close((int)fd);
        if (nr <= 0) continue;
        buf[nr] = '\0';
        const char* r = buf;
        while (*r) {
            /* Target line: starts at col 0, ends with `:` */
            if (*r == ' ' || *r == '\t' || *r == '.' || *r == '\n') {
                while (*r && *r != '\n') r++;
                if (*r == '\n') r++;
                continue;
            }
            const char* tn = r;
            uint32_t tlen = 0;
            while (*r && *r != ':' && *r != ' ' && *r != '\t' && *r != '\n' && tlen < COMP_MAX_NAME) {
                r++; tlen++;
            }
            if (*r == ':' && tlen > 0) {
                char tgt[COMP_MAX_NAME];
                for (uint32_t ti = 0; ti < tlen; ti++) tgt[ti] = tn[ti];
                tgt[tlen] = '\0';
                if (plen == 0 || my_strncmp(tgt, prefix, plen) == 0)
                    cand_add(cs, nc, tgt, 0);
            }
            while (*r && *r != '\n') r++;
            if (*r == '\n') r++;
        }
        return; /* Found a Makefile */
    }
}

static void collect_contextual(jash_state_t* s, const char* cmd, const char* prefix,
                               uint32_t plen, cand_t* cs, int* nc) {
    static const char* npm[] = { "install", "run", "test", "start", "build", "publish", "--help", NULL };
    static const char* cargo[] = { "build", "check", "clippy", "fmt", "run", "test", "update", "--release", "--help", NULL };
    static const char* makew[] = { "build", "debug", "run", "clean", "test", "-j", "--help", NULL };
    static const char* docker[] = { "build", "compose", "exec", "images", "logs", "ps", "pull", "push", "run", "stop", "--help", NULL };
    static const char* abbr[] = { "--global", "--local", "--command", "--erase", "-e", NULL };
    static const char* sysctl[] = { "start", "stop", "restart", "reload", "status", "enable", "disable", "is-active", "is-enabled", "list-units", "daemon-reload", "show", "cat", "edit", "--help", NULL };
    static const char* brewcmd[] = { "install", "uninstall", "update", "upgrade", "list", "search", "info", "doctor", "cleanup", "outdated", "pin", "unpin", "tap", "untap", "services", "--help", NULL };
    static const char* launchctl_cmds[] = { "load", "unload", "start", "stop", "list", "kickstart", "kill", "enable", "disable", "submit", "bootstrap", "bootout", "print", "print-cache", "blame", "--help", NULL };
    static const char* generic[] = { "--help", "-h", "--version", "-v", NULL };

    if (my_streq(cmd, "git")) { collect_git_state_aware(prefix, plen, cs, nc); return; }
    else if (my_streq(cmd, "killall") || my_streq(cmd, "pkill") || my_streq(cmd, "kill")) {
        collect_proc_names(prefix, plen, cs, nc); return;
    }
    else if (my_streq(cmd, "npm")) collect_words(npm, prefix, plen, cs, nc);
    else if (my_streq(cmd, "cargo")) collect_words(cargo, prefix, plen, cs, nc);
    else if (my_streq(cmd, "make")) {
        collect_words(makew, prefix, plen, cs, nc);
        if (*nc == 0) collect_make_targets(prefix, plen, cs, nc);
    }
    else if (my_streq(cmd, "docker")) collect_words(docker, prefix, plen, cs, nc);
    else if (my_streq(cmd, "theme")) { collect_theme_names(prefix, plen, cs, nc); return; }
    else if (my_streq(cmd, "abbr")) collect_words(abbr, prefix, plen, cs, nc);
    else if (my_streq(cmd, "systemctl")) collect_words(sysctl, prefix, plen, cs, nc);
    else if (my_streq(cmd, "brew")) collect_words(brewcmd, prefix, plen, cs, nc);
    else if (my_streq(cmd, "launchctl")) collect_words(launchctl_cmds, prefix, plen, cs, nc);
    else if (my_streq(cmd, "rsync")) {
        /* Collect SSH hosts for rsync remote targets */
        collect_ssh_hosts(s, prefix, plen, cs, nc);
        if (*nc > 0) return;
    }
    else if (my_streq(cmd, "ssh") || my_streq(cmd, "scp")) {
        collect_ssh_hosts(s, prefix, plen, cs, nc);
        if (*nc > 0) return;
    }
    collect_words(generic, prefix, plen, cs, nc);
}

/* ---- directory enumeration ---- */
static void collect_dir(const char* dirpath, const char* prefix,
                        uint32_t plen, cand_t* cs, int* nc) {
    DIR *dir = opendir(dirpath);
    if (!dir) return;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        const char *nm = entry->d_name;
        if (nm[0] == '.' && (nm[1] == '\0' || (nm[1] == '.' && nm[2] == '\0')))
            continue;
        if (plen == 0 || my_strncmp(nm, prefix, plen) == 0) {
            int is_dir = 0;
            #ifdef __APPLE__
            is_dir = (entry->d_type == DT_DIR);
            #elif defined(_DIRENT_HAVE_D_TYPE)
            is_dir = (entry->d_type == DT_DIR);
            #else
            if (entry->d_type == DT_UNKNOWN) {
                /* Fallback to stat only when d_type is unavailable */
                is_dir = path_is_dir(nm);
            }
            #endif
            cand_add(cs, nc, nm, is_dir);
        }
    }
    closedir(dir);
}

static void collect_text_dir(const char* dirpath, const char* prefix,
                             uint32_t plen, cand_t* cs, int* nc) {
    DIR *dir = opendir(dirpath);
    if (!dir) return;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        const char *nm = entry->d_name;
        if (nm[0] == '.' && (nm[1] == '\0' || (nm[1] == '.' && nm[2] == '\0')))
            continue;
        if (is_search_ignored_name(nm)) continue;
        if (plen != 0 && my_strncmp(nm, prefix, plen) != 0) continue;
        if (path_is_regular(nm) && is_textish_name(nm))
            cand_add(cs, nc, nm, 0);
    }
    closedir(dir);
}

static void collect_reader_dir(const char* dirpath, const char* prefix,
                               uint32_t plen, cand_t* cs, int* nc) {
    DIR *dir = opendir(dirpath);
    if (!dir) return;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        const char *nm = entry->d_name;
        if (nm[0] == '.' && (nm[1] == '\0' || (nm[1] == '.' && nm[2] == '\0')))
            continue;
        if (is_search_ignored_name(nm)) continue;
        if (plen != 0 && my_strncmp(nm, prefix, plen) != 0) continue;
        if (path_is_dir(nm)) cand_add(cs, nc, nm, 1);
        else if (path_is_regular(nm) && is_textish_name(nm) && access(nm, R_OK) == 0)
            cand_add(cs, nc, nm, 0);
    }
    closedir(dir);
}

/* ---- longest common prefix ---- */
static uint32_t lcp_len(const cand_t* cs, int nc) {
    if (nc <= 0) return 0;
    if (nc == 1) return my_slen(cs[0].name);
    uint32_t k = 0;
    for (;;) {
        char c = cs[0].name[k];
        if (!c) return k;
        for (int i = 1; i < nc; i++) {
            if (cs[i].name[k] != c) return k;
        }
        k++;
    }
}

/* ---- cycling state ---- */
static int g_cycle_idx = -1;       /* last selected candidate index */
static char g_cycle_prefix[COMP_MAX_NAME]; /* what was typed when cycling started */

static struct {
    int active;
    cand_t cs[COMP_MAX_CANDS];
    int nc;
    int selected;
    int visible_lines;
    int cols;           /* columns in the current grid (set by render_pager) */
    uint32_t token_start;
    char prefix[COMP_MAX_NAME];
    uint32_t plen;
} g_pager;

static void pager_reset(void) {
    g_pager.active = 0;
    g_pager.nc = 0;
    g_pager.selected = 0;
    g_pager.visible_lines = 0;
    g_pager.token_start = 0;
    g_pager.prefix[0] = 0;
    g_pager.plen = 0;
}

/* Reset cycling state (call when cursor moves or text changes) */
void jash_complete_reset_cycle(void) {
    g_cycle_idx = -1;
    g_cycle_prefix[0] = '\0';
    pager_reset();
}

static void replace_current_token(jash_line_t* L, uint32_t ts, uint32_t plen,
                                  const cand_t* c) {
    if (!L || !c) return;
    uint32_t end = ts + plen;
    if (end > L->len) end = L->len;
    uint32_t tail_len = L->len - end;
    char tail[JASH_LINE_MAX];
    for (uint32_t i = 0; i < tail_len && i < sizeof(tail); i++)
        tail[i] = L->buf[end + i];

    uint32_t clen = my_slen(c->name);
    uint32_t nlen = ts;
    for (uint32_t i = 0; i < clen && nlen + 1 < JASH_LINE_MAX; i++)
        L->buf[nlen++] = c->name[i];
    if (nlen + 1 < JASH_LINE_MAX)
        L->buf[nlen++] = c->is_dir ? '/' : ' ';
    for (uint32_t i = 0; i < tail_len && nlen + 1 < JASH_LINE_MAX; i++)
        L->buf[nlen++] = tail[i];
    L->len = nlen;
    L->cur = ts + clen + 1;
    if (L->cur > L->len) L->cur = L->len;
    L->buf[L->len] = 0;
}

/* Visible terminal width for the completion grid. TIOCGWINSZ → $COLUMNS → 80. */
static int pager_term_width(void) {
#ifdef JASH_TINALINUX
    unsigned columns = 0;
    if (jash_tina_surface_native_output()) {
        return jash_tina_surface_viewport(&columns, NULL) == 0 ? (int)columns : 80;
    }
#endif
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return (int)ws.ws_col;
    const char* c = getenv("COLUMNS");
    if (c && *c) { int v = atoi(c); if (v > 0) return v; }
    return 80;
}

#define PAGER_MAX_ROWS 14   /* inline overlay above the prompt — bounded */

static void render_pager(jash_state_t* s, jash_writer_t w, void* wctx,
                         const jash_line_t* L) {
    if (!g_pager.active || g_pager.nc <= 0) return;

    /* Longest candidate (name + trailing '/' for dirs) → column width. */
    int maxlen = 1;
    for (int i = 0; i < g_pager.nc; i++) {
        int l = (int)my_slen(g_pager.cs[i].name) + (g_pager.cs[i].is_dir ? 1 : 0);
        if (l > maxlen) maxlen = l;
    }

    /* Pack as many columns as the terminal width allows (full-width grid). */
    const int gap = 2;
    int tw = pager_term_width();
    int col_w = maxlen + gap;
    int cols = tw / col_w;
    if (cols < 1) cols = 1;
    if (cols > g_pager.nc) cols = g_pager.nc;
    g_pager.cols = cols;

    int rows = (g_pager.nc + cols - 1) / cols;
    int truncated = 0;
    if (rows > PAGER_MAX_ROWS) { rows = PAGER_MAX_ROWS; truncated = 1; }
    int max_items = rows * cols;
    int visible_lines = 1 + rows + (truncated ? 1 : 0);

    /* Wipe the previous overlay. */
    if (g_pager.visible_lines > 0) {
        jash_writes(w, wctx, "\0337");
        jash_writef(w, wctx, "\033[%dA", g_pager.visible_lines);
        for (int i = 0; i < g_pager.visible_lines; i++) {
            jash_writes(w, wctx, "\r\033[2K");
            if (i + 1 < g_pager.visible_lines) jash_writes(w, wctx, "\r\n");
        }
        jash_writes(w, wctx, "\0338");
    }

    jash_writes(w, wctx, "\0337");
    jash_writef(w, wctx, "\033[%dA", visible_lines);
    jash_writes(w, wctx, "\r\033[2K\033[1;36mcompletions\033[0m ");
    jash_writef(w, wctx, "\033[90m%d items\033[0m\r\n", g_pager.nc);

    /* Row-major aligned grid: each cell padded to col_w so columns line up. */
    for (int i = 0; i < g_pager.nc && i < max_items; i++) {
        if (i % cols == 0) jash_writes(w, wctx, "\r\033[2K");
        int namew = (int)my_slen(g_pager.cs[i].name) + (g_pager.cs[i].is_dir ? 1 : 0);
        if (i == g_pager.selected) jash_writes(w, wctx, "\033[30;47m");
        else if (g_pager.cs[i].is_dir) jash_writes(w, wctx, "\033[34m");
        else jash_writes(w, wctx, "\033[32m");
        jash_writes_sanitized(w, wctx, g_pager.cs[i].name);
        if (g_pager.cs[i].is_dir) jash_writes(w, wctx, "/");
        jash_writes(w, wctx, "\033[0m");
        if ((i + 1) % cols == 0) {
            jash_writes(w, wctx, "\r\n");
        } else {
            /* pad to align the next column */
            write_pad(w, wctx, col_w - namew);
        }
    }
    if (truncated) {
        jash_writes(w, wctx, "\r\n\033[2K\033[90m");
        jash_writef(w, wctx, "+%d more (type to narrow)\033[0m", g_pager.nc - max_items);
    }
    jash_writes(w, wctx, "\0338");
    g_pager.visible_lines = visible_lines;
    jash_render_line(w, wctx, s, L);
}

static int build_candidates(jash_state_t* s, const jash_line_t* L,
                            uint32_t* out_ts, uint32_t* out_plen,
                            char* prefix, cand_t* cs, int* nc) {
    uint32_t ts = token_start(L);
    uint32_t plen = L->cur - ts;
    if (plen >= COMP_MAX_NAME) return 0;
    for (uint32_t i = 0; i < plen; i++) prefix[i] = L->buf[ts + i];
    prefix[plen] = '\0';
    *nc = 0;

    /* $VAR completion */
    if (plen > 0 && prefix[0] == '$') {
        uint32_t varlen = plen - 1;
        const char* varpart = prefix + 1;
        /* Check for ${VAR} syntax */
        uint32_t skip = 0;
        if (varlen > 0 && varpart[0] == '{') { varpart++; varlen--; skip = 1; }
        uint32_t env_count = jash_env_count(s);
        for (uint32_t ei = 0; ei < env_count; ei++) {
            const char* en = s->env[ei].key;
            if (varlen == 0 || (my_slen(en) >= varlen && my_strncmp(en, varpart, varlen) == 0)) {
                char display[COMP_MAX_NAME];
                uint32_t dk = 0;
                display[dk++] = '$';
                if (skip) display[dk++] = '{';
                for (uint32_t mi = 0; en[mi] && dk + 1 < sizeof(display); mi++)
                    display[dk++] = en[mi];
                if (skip && dk + 1 < sizeof(display)) display[dk++] = '}';
                display[dk] = '\0';
                cand_add(cs, nc, display, 0);
            }
        }
        if (*nc > 0) { *out_ts = ts; *out_plen = plen; return 1; }
    }

    int first = is_first_token(L, ts);
    if (first) {
        collect_builtins(prefix, plen, cs, nc);
        const char* path = jash_env_get(s, "PATH");
        if (!path) path = "/bin:/usr/bin:/usr/local/bin";
        const char* p = path;
        while (*p) {
            const char* start = p;
            while (*p && *p != ':') p++;
            uint32_t dirlen = (uint32_t)(p - start);
            if (dirlen > 0) {
                char dirpath[256];
                uint32_t k = 0;
                for (uint32_t i = 0; i < dirlen && k + 1 < sizeof(dirpath); i++)
                    dirpath[k++] = start[i];
                dirpath[k] = '\0';
                collect_dir(dirpath, prefix, plen, cs, nc);
            }
            if (*p == ':') p++;
        }
    } else {
        char cmd[COMP_MAX_NAME];
        first_token_name(L, cmd, sizeof(cmd));
        
        /* Passthrough commands: complete the next token as a command name */
        if (is_passthrough_cmd(cmd)) {
            /* Determine the token index. If token 1 → command completion, else → contextual */
            uint32_t ti = 0, pos = 0;
            while (pos < L->len && (L->buf[pos] == ' ' || L->buf[pos] == '\t')) pos++;
            while (pos < ts) {
                while (pos < L->len && L->buf[pos] != ' ' && L->buf[pos] != '\t') pos++;
                while (pos < L->len && (L->buf[pos] == ' ' || L->buf[pos] == '\t')) pos++;
                ti++;
            }
            /* If ti == 1 (second token), do command completion */
            if (ti == 1) {
                collect_builtins(prefix, plen, cs, nc);
                goto path_complete;
            }
            /* Otherwise, use the real command (token 1) for contextual completion */
            /* Extract the real command (token 1) */
            uint32_t rp = 0;
            while (rp < L->len && (L->buf[rp] == ' ' || L->buf[rp] == '\t')) rp++;
            while (rp < L->len && L->buf[rp] != ' ' && L->buf[rp] != '\t') rp++;
            while (rp < L->len && (L->buf[rp] == ' ' || L->buf[rp] == '\t')) rp++;
            uint32_t rc = 0;
            while (rp < L->len && L->buf[rp] != ' ' && L->buf[rp] != '\t' && rc + 1 < sizeof(cmd))
                cmd[rc++] = L->buf[rp++];
            cmd[rc] = '\0';
        }
        
        /* Check if command has registered completion words */
        const char* wordlist = jash_complete_get_words(cmd);
        if (wordlist && *wordlist) {
            /* Parse wordlist and add matching candidates */
            const char* w = wordlist;
            while (*w) {
                while (*w == ' ') w++;
                if (!*w) break;
                const char* start = w;
                while (*w && *w != ' ') w++;
                uint32_t wlen = (uint32_t)(w - start);
                if (wlen >= COMP_MAX_NAME) continue;
                
                char word[COMP_MAX_NAME];
                for (uint32_t i = 0; i < wlen; i++) word[i] = start[i];
                word[wlen] = '\0';

                /* Check if word matches prefix */
                if (plen == 0 || my_strncmp(word, prefix, plen) == 0) {
                    cand_add(cs, nc, word, 0);
                }
            }
            if (*nc > 0) goto done;
        }

        /* Check for registered completion action (-A) */
        const char* action = jash_complete_get_action(cmd);
        if (action) {
            if (my_streq(action, "signal")) {
                static const char* sigs[] = {
                    "SIGABRT","SIGALRM","SIGBUS","SIGCHLD","SIGCONT",
                    "SIGFPE","SIGHUP","SIGILL","SIGINT","SIGKILL",
                    "SIGPIPE","SIGQUIT","SIGSEGV","SIGSTOP","SIGTERM",
                    "SIGTSTP","SIGTTIN","SIGTTOU","SIGUSR1","SIGUSR2",
                    "SIGSYS","SIGTRAP","SIGURG","SIGXCPU","SIGXFSZ",
                    "SIGVTALRM","SIGPROF","SIGWINCH","SIGIO","SIGPWR",
                    0
                };
                for (int si = 0; sigs[si]; si++) {
                    int slen = (int)my_slen(sigs[si]);
                    if (plen == 0 || (slen >= (int)plen &&
                        my_strncmp(sigs[si], prefix, plen) == 0)) {
                        cand_add(cs, nc, sigs[si], 0);
                    }
                }
                goto done;
            }
            if (my_streq(action, "hostname")) {
                char buf[4096];
                long fd = sys_open("/etc/hosts", 0, 0);
                if (fd >= 0) {
                    long nr = sys_read((int)fd, buf, sizeof(buf) - 1);
                    sys_close((int)fd);
                    if (nr > 0) {
                        buf[nr] = '\0';
                        const char* q = buf;
                        while (*q) {
                            if (*q == '#' || *q == '\n') { while (*q && *q != '\n') q++; if (*q) q++; continue; }
                            while (*q && *q != ' ' && *q != '\t') q++;
                            while (*q == ' ' || *q == '\t') q++;
                            while (*q && *q != '\n' && *q != '#') {
                                const char* hn = q;
                                while (*q && *q != ' ' && *q != '\t' && *q != '\n' && *q != '#') q++;
                                uint32_t hnlen = (uint32_t)(q - hn);
                                if (hnlen > 0 && hnlen < COMP_MAX_NAME) {
                                    char hnbuf[COMP_MAX_NAME];
                                    for (uint32_t hni = 0; hni < hnlen; hni++) hnbuf[hni] = hn[hni];
                                    hnbuf[hnlen] = '\0';
                                    if (plen == 0 || my_strncmp(hnbuf, prefix, plen) == 0)
                                        cand_add(cs, nc, hnbuf, 0);
                                }
                                while (*q == ' ' || *q == '\t') q++;
                            }
                            if (*q == '#') while (*q && *q != '\n') q++;
                            if (*q == '\n') q++;
                        }
                    }
                }
                goto done;
            }
            if (my_streq(action, "user")) {
                char buf[4096];
                long fd = sys_open("/etc/passwd", 0, 0);
                if (fd >= 0) {
                    long nr = sys_read((int)fd, buf, sizeof(buf) - 1);
                    sys_close((int)fd);
                    if (nr > 0) {
                        buf[nr] = '\0';
                        const char* q = buf;
                        while (*q) {
                            const char* ul = q;
                            while (*q && *q != ':') q++;
                            uint32_t ulen = (uint32_t)(q - ul);
                            if (ulen > 0 && ulen < COMP_MAX_NAME) {
                                char ubuf[COMP_MAX_NAME];
                                for (uint32_t ui = 0; ui < ulen; ui++) ubuf[ui] = ul[ui];
                                ubuf[ulen] = '\0';
                                if (plen == 0 || my_strncmp(ubuf, prefix, plen) == 0)
                                    cand_add(cs, nc, ubuf, 0);
                            }
                            while (*q && *q != '\n') q++;
                            if (*q == '\n') q++;
                        }
                    }
                }
                goto done;
            }
            if (my_streq(action, "running")) {
                char psbuf[16384];
                int fds[2];
                if (sys_pipe(fds) == 0) {
                    long pid = sys_fork();
                    if (pid == 0) {
                        sys_close(fds[0]);
                        sys_dup2(fds[1], 1);
                        sys_close(fds[1]);
                        const char* argv[] = {"/bin/ps", "-e", "-o", "comm=", 0};
                        sys_execve("/bin/ps", (char* const*)argv, 0);
                        sys_exit(1);
                    } else if (pid > 0) {
                        sys_close(fds[1]);
                        long nr = sys_read(fds[0], psbuf, sizeof(psbuf) - 1);
                        sys_close(fds[0]);
                        sys_waitpid(pid, 0, 0);
                        if (nr > 0) {
                            psbuf[nr] = '\0';
                            const char* q = psbuf;
                            while (*q) {
                                while (*q == ' ' || *q == '\n') q++;
                                if (!*q) break;
                                const char* pn = q;
                                while (*q && *q != '\n') q++;
                                uint32_t pl = (uint32_t)(q - pn);
                                if (pl > 0 && pl < COMP_MAX_NAME) {
                                    char pbuf[COMP_MAX_NAME];
                                    for (uint32_t pi = 0; pi < pl; pi++) pbuf[pi] = pn[pi];
                                    pbuf[pl] = '\0';
                                    if (plen == 0 || my_strncmp(pbuf, prefix, plen) == 0)
                                        cand_add(cs, nc, pbuf, 0);
                                }
                            }
                        }
                    }
                }
                goto done;
            }
        }
        
        if (is_io_path_cmd(cmd) && prefix[0] != '-') {
            if (my_streq(cmd, "grep")) collect_text_dir(".", prefix, plen, cs, nc);
            else if (is_reader_cmd(cmd)) collect_reader_dir(".", prefix, plen, cs, nc);
            else collect_dir(".", prefix, plen, cs, nc);
            if (*nc > 0) goto done;
        }
        collect_contextual(s, cmd, prefix, plen, cs, nc);
        if (*nc > 0) goto done;

        /* Dynamic flag completion: parse command --help for flags */
        if (prefix[0] == '-' && *nc == 0) {
            const char* cached = flag_cache_lookup(cmd);
            if (cached) {
                /* Parse cached flags */
                const char* q = cached;
                while (*q) {
                    const char* flag = q;
                    while (*q && *q != ' ') q++;
                    uint32_t flaglen = (uint32_t)(q - flag);
                    if (flaglen > 0 && flaglen < COMP_MAX_NAME) {
                        char flagbuf[COMP_MAX_NAME];
                        for (uint32_t fi = 0; fi < flaglen; fi++) flagbuf[fi] = flag[fi];
                        flagbuf[flaglen] = '\0';
                        if (plen == 0 || my_strncmp(flagbuf, prefix, plen) == 0)
                            cand_add(cs, nc, flagbuf, 0);
                    }
                    if (*q == ' ') q++;
                }
            } else {
                char helpbuf[16384];
                char flag_store[2048];
                uint32_t fs = 0;
                int probed = 0;
                int fds[2];
                /* sys_execve does no PATH lookup: a bare 'git' always failed
                 * with the child exiting 127, so the fork repeated on every
                 * TAB. Resolve via PATH (execvp-style) and cache failures. */
                const char* hpath = jash_env_get(s, "PATH");
                if (!hpath) hpath = "/bin:/usr/bin:/usr/local/bin";
                if (sys_pipe(fds) == 0) {
                    long pid = sys_fork();
                    if (pid == 0) {
                        sys_close(fds[0]);
                        sys_dup2(fds[1], 2);
                        sys_dup2(fds[1], 1);
                        sys_close(fds[1]);
                        const char* argv[] = {cmd, "--help", 0};
                        int has_slash = 0;
                        for (uint32_t ci = 0; cmd[ci]; ci++)
                            if (cmd[ci] == '/') { has_slash = 1; break; }
                        if (has_slash) {
                            sys_execve(cmd, (char* const*)argv, 0);
                        } else {
                            const char* p = hpath;
                            while (*p) {
                                const char* dstart = p;
                                while (*p && *p != ':') p++;
                                uint32_t dlen = (uint32_t)(p - dstart);
                                if (dlen > 0) {
                                    char exe[512];
                                    uint32_t k = 0;
                                    for (uint32_t di = 0; di < dlen && k + 1 < sizeof(exe); di++)
                                        exe[k++] = dstart[di];
                                    if (k + 1 < sizeof(exe)) exe[k++] = '/';
                                    for (uint32_t ci = 0; cmd[ci] && k + 1 < sizeof(exe); ci++)
                                        exe[k++] = cmd[ci];
                                    exe[k] = '\0';
                                    sys_execve(exe, (char* const*)argv, 0);
                                }
                                if (*p == ':') p++;
                            }
                        }
                        sys_exit(127);
                    } else if (pid > 0) {
                        probed = 1;
                        sys_close(fds[1]);
                        long nr = sys_read(fds[0], helpbuf, sizeof(helpbuf) - 1);
                        sys_close(fds[0]);
                        sys_waitpid(pid, 0, 0);
                        if (nr > 0) {
                            helpbuf[nr] = '\0';
                            const char* q = helpbuf;
                            while (*q) {
                                const char* line = q;
                                while (*q && *q != '\n') q++;
                                uint32_t llen = (uint32_t)(q - line);
                                if (*q == '\n') q++;
                                if (llen == 0) continue;
                                const char* fp = line;
                                while (fp < line + llen && (*fp == ' ' || *fp == '\t')) fp++;
                                if (fp < line + llen && *fp == '-') {
                                    const char* flag = fp;
                                    while (fp < line + llen && *fp != ' ' && *fp != '\t' && *fp != ',' && *fp != '=') fp++;
                                    uint32_t flaglen = (uint32_t)(fp - flag);
                                    if (flaglen > 0 && flaglen < COMP_MAX_NAME && flaglen <= llen) {
                                        char flagbuf[COMP_MAX_NAME];
                                        for (uint32_t fi = 0; fi < flaglen; fi++) flagbuf[fi] = flag[fi];
                                        flagbuf[flaglen] = '\0';
                                        if (plen == 0 || my_strncmp(flagbuf, prefix, plen) == 0)
                                            cand_add(cs, nc, flagbuf, 0);
                                        /* Store in cache */
                                        for (uint32_t fi = 0; fi < flaglen && fs + 1 < sizeof(flag_store); fi++)
                                            flag_store[fs++] = flagbuf[fi];
                                        if (fs + 1 < sizeof(flag_store)) flag_store[fs++] = ' ';
                                    }
                                }
                            }
                        }
                    }
                }
                if (fs > 0) {
                    if (flag_store[fs - 1] == ' ') fs--;
                    flag_store[fs] = '\0';
                    flag_cache_store(cmd, flag_store);
                } else if (probed) {
                    /* Negative cache: the probe ran but produced no flags —
                     * don't refork on every TAB (expires via FLAG_NEG_TTL_SEC). */
                    flag_cache_store(cmd, "");
                }
            }
            if (*nc > 0) goto done;
        }

        const char* slash = 0;
        for (int i = (int)plen - 1; i >= 0; i--) {
            if (prefix[i] == '/') { slash = &prefix[i]; break; }
        }
        if (slash) {
            char dirpath[COMP_MAX_NAME];
            uint32_t dl = (uint32_t)(slash - prefix);
            if (dl == 0) { dirpath[0] = '/'; dirpath[1] = '\0'; }
            else {
                for (uint32_t i = 0; i < dl && i + 1 < sizeof(dirpath); i++)
                    dirpath[i] = prefix[i];
                dirpath[dl] = '\0';
            }
            const char* fprefix = slash + 1;
            uint32_t fplen = my_slen(fprefix);
            cand_t local[COMP_MAX_CANDS];
            int lnc = 0;
            collect_dir(dirpath, fprefix, fplen, local, &lnc);
            for (int i = 0; i < lnc; i++) {
                char joined[COMP_MAX_NAME];
                uint32_t k = 0;
                for (uint32_t j = 0; j < dl && k + 1 < sizeof(joined); j++)
                    joined[k++] = prefix[j];
                if (k + 1 < sizeof(joined)) joined[k++] = '/';
                for (uint32_t j = 0; local[i].name[j] && k + 1 < sizeof(joined); j++)
                    joined[k++] = local[i].name[j];
                joined[k] = '\0';
                cand_add(cs, nc, joined, local[i].is_dir);
            }
        } else {
            collect_dir(".", prefix, plen, cs, nc);
        }
    }

path_complete:
    /* Common path completion: scan PATH for executables */
    if (*nc == 0) {
        const char* path = jash_env_get(s, "PATH");
        if (!path) path = "/bin:/usr/bin:/usr/local/bin";
        const char* p = path;
        while (*p) {
            const char* start = p;
            while (*p && *p != ':') p++;
            uint32_t dirlen = (uint32_t)(p - start);
            if (dirlen > 0) {
                char dirpath[256];
                uint32_t k = 0;
                for (uint32_t i = 0; i < dirlen && k + 1 < sizeof(dirpath); i++)
                    dirpath[k++] = start[i];
                dirpath[k] = '\0';
                collect_dir(dirpath, prefix, plen, cs, nc);
            }
            if (*p == ':') p++;
        }
    }

done:
    *out_ts = ts;
    *out_plen = plen;
    return *nc > 0;
}

int jash_complete_pager_key(jash_state_t* s, jash_writer_t w, void* wctx,
                            jash_line_t* L, int key) {
    if (!g_pager.active) return 0;
    int cols = g_pager.cols > 0 ? g_pager.cols : 1;
    if (key == JK_ESC || key == JK_CTRL_C) {
        pager_reset();
        jash_render_line(w, wctx, s, L);
        return 1;
    }
    if (key == JK_ENTER) {
        replace_current_token(L, g_pager.token_start, g_pager.plen,
                              &g_pager.cs[g_pager.selected]);
        pager_reset();
        jash_render_line(w, wctx, s, L);
        return 1;
    }
    if (key == JK_TAB || key == JK_RIGHT || key == JK_DOWN) {
        int step = (key == JK_DOWN) ? cols : 1;
        g_pager.selected = (g_pager.selected + step) % g_pager.nc;
        render_pager(s, w, wctx, L);
        return 1;
    }
    if (key == JK_LEFT || key == JK_UP) {
        int step = (key == JK_UP) ? cols : 1;
        g_pager.selected -= step;
        while (g_pager.selected < 0) g_pager.selected += g_pager.nc;
        render_pager(s, w, wctx, L);
        return 1;
    }
    return 0;
}

int jash_complete_pager_active(void) {
    return g_pager.active;
}

static void command_name_from_line(const jash_line_t* L, char* out, uint32_t cap) {
    uint32_t i = 0, k = 0;
    while (i < L->len && (L->buf[i] == ' ' || L->buf[i] == '\t')) i++;
    while (i < L->len && L->buf[i] != ' ' && L->buf[i] != '\t' && k + 1 < cap)
        out[k++] = L->buf[i++];
    out[k] = 0;
}

static void collect_hud_dir(cand_t* cs, int* nc, int mode) {
    DIR* dir = opendir(".");
    if (!dir) return;
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        const char* nm = entry->d_name;
        if (nm[0] == '.' && (nm[1] == '\0' || (nm[1] == '.' && nm[2] == '\0')))
            continue;
        if (mode == 1) {
            cand_add(cs, nc, nm, path_is_dir(nm));
        } else if (mode == 2) {
            if (path_is_regular(nm) && is_textish_name(nm) && access(nm, R_OK) == 0)
                cand_add(cs, nc, nm, 0);
        } else if (mode == 3) {
            if (path_is_dir(nm)) cand_add(cs, nc, nm, 1);
        } else if (mode == 4) {
            if (is_search_ignored_name(nm)) continue;
            if (path_is_regular(nm) && is_textish_name(nm) && access(nm, R_OK) == 0)
                cand_add(cs, nc, nm, 0);
        }
    }
    closedir(dir);
}

void jash_complete_command_hud(jash_state_t* s, jash_writer_t w, void* wctx,
                               const jash_line_t* L) {
    (void)s;
    if (!w || !L) return;

    char cmd[COMP_MAX_NAME];
    command_name_from_line(L, cmd, sizeof(cmd));
    if (!cmd[0]) return;

    cand_t cs[COMP_MAX_CANDS];
    int nc = 0;
    const char* title = "Command HUD";

    if (my_streq(cmd, "ls")) {
        title = "ls sees directory entries";
        collect_hud_dir(cs, &nc, 1);
    } else if (my_streq(cmd, "cat") || my_streq(cmd, "less") ||
               my_streq(cmd, "more") || my_streq(cmd, "head") ||
               my_streq(cmd, "tail")) {
        title = "reader-safe text files";
        collect_hud_dir(cs, &nc, 2);
    } else if (my_streq(cmd, "cd") || my_streq(cmd, "pushd")) {
        title = "jumpable directories";
        collect_hud_dir(cs, &nc, 3);
    } else if (my_streq(cmd, "grep")) {
        title = "grep-safe text files";
        collect_hud_dir(cs, &nc, 4);
    } else {
        title = "files for this command";
        collect_hud_dir(cs, &nc, 1);
    }

    jash_writes(w, wctx, "\r\n\033[1;36m");
    jash_writes_sanitized(w, wctx, title);
    jash_writes(w, wctx, "\033[0m\r\n");
    if (nc == 0) {
        jash_writes(w, wctx, "  \033[90m(no semantic candidates)\033[0m\r\n");
        jash_render_line(w, wctx, s, L);
        return;
    }

    for (int i = 0; i < nc; i++) {
        if (cs[i].is_dir) jash_writes(w, wctx, "\033[94m");
        else jash_writes(w, wctx, "\033[92m");
        jash_writes_sanitized(w, wctx, cs[i].name);
        if (cs[i].is_dir) jash_writes(w, wctx, "/");
        jash_writes(w, wctx, "\033[0m");
        jash_writes(w, wctx, (i + 1) % 4 == 0 ? "\r\n" : "  ");
    }
    jash_writes(w, wctx, "\r\n");
    jash_render_line(w, wctx, s, L);
}

/* ---- main entry ---- */
int jash_complete_tab(jash_state_t* s, jash_writer_t w, void* wctx,
                      jash_line_t* L) {
    if (jash_complete_pager_key(s, w, wctx, L, JK_TAB)) return 1;

    uint32_t ts = 0;
    uint32_t plen = 0;
    char prefix[COMP_MAX_NAME];
    cand_t cs[COMP_MAX_CANDS];
    int nc = 0;
    build_candidates(s, L, &ts, &plen, prefix, cs, &nc);
    if (nc == 0) {
        jash_complete_reset_cycle();
        return 0;
    }

    if (nc == 1) {
        /* Unique completion */
        jash_complete_reset_cycle();

        /* Directory completion must stop at the directory itself. Opening a
         * child pager here makes Enter accept the first child instead of
         * executing commands like `cd dir/`. Press Tab again to inspect inside. */
        if (cs[0].is_dir && plen > 0 && prefix[plen - 1] != '/') {
            replace_current_token(L, ts, plen, &cs[0]);
            jash_render_line(w, wctx, s, L);
            return 1;
        }

        replace_current_token(L, ts, plen, &cs[0]);
        jash_render_line(w, wctx, s, L);
        return 1;
    }

    char cmd[COMP_MAX_NAME];
    first_token_name(L, cmd, sizeof(cmd));
    if (!is_first_token(L, ts) && is_io_path_cmd(cmd)) goto open_pager;

    /* Multiple candidates: try LCP extension first. */
    uint32_t lcp = lcp_len(cs, nc);
    if (lcp > plen) {
        jash_complete_reset_cycle();
        while (L->cur > ts) { L->cur--; L->len--; }
        jash_render_line(w, wctx, s, L);
        for (uint32_t i = 0; i < lcp; i++) {
            if (L->len + 1 >= JASH_LINE_MAX) break;
            L->buf[L->len++] = cs[0].name[i];
            L->cur++;
        }
        {   char lcp_buf[256];
            uint32_t n = lcp < 255 ? lcp : 255;
            for (uint32_t i = 0; i < n; i++) lcp_buf[i] = cs[0].name[i];
            lcp_buf[n] = 0;
            jash_writes_sanitized(w, wctx, lcp_buf);
        }
        return 1;
    }

open_pager:
    pager_reset();
    g_pager.active = 1;
    g_pager.nc = nc;
    g_pager.selected = 0;
    g_pager.token_start = ts;
    g_pager.plen = plen;
    for (uint32_t i = 0; i < plen && i + 1 < sizeof(g_pager.prefix); i++)
        g_pager.prefix[i] = prefix[i];
    g_pager.prefix[plen < COMP_MAX_NAME ? plen : COMP_MAX_NAME - 1] = 0;
    for (int i = 0; i < nc; i++) {
        g_pager.cs[i] = cs[i];
    }
    render_pager(s, w, wctx, L);
    return 1;
}
