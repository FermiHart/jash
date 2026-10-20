// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — core/err_digest — compact error digest for noisy build tools

#include "err_digest.h"
#include <core/memo.h>   /* jash_env_smells_like_agent (ADR-0037) */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>

#define DIGEST_MAX      20
#define DIGEST_LINE_MAX 256

static const char* path_basename(const char* path) {
    const char* last = path;
    for (const char* p = path; *p; p++)
        if (*p == '/') last = p + 1;
    return last;
}

int jash_err_digest_is_noisy_tool(const char* fullpath) {
    if (!fullpath) return 0;
    const char* b = path_basename(fullpath);

    static const char* exact[] = {
        "cmake", "make", "gmake", "ninja", "ninja-build",
        "cargo", "rustc", "npm", "yarn", "pnpm", "tsc", "go",
        "cc", "c++",
        NULL
    };
    for (int i = 0; exact[i]; i++)
        if (strcmp(b, exact[i]) == 0) return 1;

    // Versioned compilers: gcc-12, g++-13, clang-15, clang++, etc.
    if (strncmp(b, "gcc", 3) == 0 && (b[3] == '\0' || b[3] == '-')) return 1;
    if (strncmp(b, "g++", 3) == 0 && (b[3] == '\0' || b[3] == '-')) return 1;
    if (strncmp(b, "clang", 5) == 0 &&
        (b[5] == '\0' || b[5] == '-' || b[5] == '+')) return 1;
    return 0;
}

static int is_error_line(const char* line) {
    if (strstr(line, ": error:"))         return 1;
    if (strstr(line, ": fatal error:"))   return 1;
    if (strncmp(line, "CMake Error", 11) == 0) return 1;
    if (strstr(line, "] Error "))         return 1;
    if (strstr(line, "make: ***"))        return 1;
    if (strncmp(line, "error[", 6) == 0)  return 1;  // cargo: error[E0...]
    if (strncmp(line, "error: ", 7) == 0) return 1;  // cargo/rustc generic
    if (strstr(line, "undefined reference to")) return 1;
    if (strstr(line, "ld: error:"))       return 1;
    return 0;
}

/* Public wrapper so the F4 salience scorer shares this exact set (ADR-0042). */
int jash_err_is_error_line(const char* line) { return line ? is_error_line(line) : 0; }

/* F12: map a known error signature to a one-line fix hint (or NULL). */
static const char* err_fix_hint(const char* line) {
    if (strstr(line, "dynamic_lookup")) return NULL;  /* already the fix */
    if (strstr(line, "Undefined symbols") || strstr(line, "undefined reference") ||
        strstr(line, "symbol(s) not found") || strstr(line, "ld: symbol"))
        return "unresolved symbol at link — add the missing source/lib; for a "
               "loadable module link with -Wl,-undefined,dynamic_lookup";
    if (strstr(line, "hex escape sequence out of range"))
        return "\\xNN greedily eats following hex digits — use octal \\NNN or "
               "split the string literal";
    if (strstr(line, "implicit declaration of function") ||
        strstr(line, "implicit function declaration") ||
        strstr(line, "call to undeclared function"))
        return "missing #include for that function (or add a prototype)";
    if (strstr(line, "use of undeclared identifier") ||
        strstr(line, "undeclared (first use"))
        return "missing #include or a typo in the identifier";
    if ((strstr(line, "fatal error:") && strstr(line, "file not found")) ||
        (strstr(line, "No such file") && strstr(line, ".h")))
        return "header not found — fix the include path (-I) or install the dev package";
    if (strstr(line, "No space left") || strstr(line, "ENOSPC"))
        return "temp/disk full — run `doctor`, or set CLAUDE_CODE_TMPDIR to a roomy dir";
    if (strstr(line, "multiple definition of"))
        return "symbol in 2 translation units — make one `static` or extern in a header";
    if (strstr(line, "Permission denied"))
        return "permission issue — check ownership/mode or choose a writable path";
    if (strstr(line, "does not appear to contain CMakeLists"))
        return "wrong source dir — point cmake -S at the folder with CMakeLists.txt";
    return NULL;
}

/* ── ADR-0038 W4: root-cause folding ────────────────────────────────────── */

static int is_warning_line(const char* line) {
    return strstr(line, ": warning:") != NULL ||
           strncmp(line, "warning:", 8) == 0;
}

/* Root-cause key: the error message with its specifics neutralized —
 * digits dropped, 'quoted'/"quoted"/`quoted` spans collapsed to <id>, and
 * for compiler-style lines the leading file:line:col: prefix removed. The
 * common cascade (one broken header hit by 30 translation units, or one
 * undeclared identifier reported once per use) folds to ONE group while
 * genuinely different causes stay separate. */
static void fold_key(const char* line, char* key, size_t cap) {
    const char* p = line;
    /* Skip a file:line:col: prefix — start at the last ": error:"-ish tag. */
    const char* tag = strstr(line, ": fatal error:");
    if (!tag) tag = strstr(line, ": error:");
    if (!tag) tag = strstr(line, "error:");
    if (tag) { p = tag; }
    size_t o = 0;
    int in_q = 0;
    char qc = 0;
    while (*p && o + 5 < cap) {
        char c = *p;
        if (in_q) {
            if (c == qc) in_q = 0;
            p++;
            continue;
        }
        if (c == '\'' || c == '"' || c == '`') {
            in_q = 1;
            qc = (c == '`') ? '\'' : c;   /* clang uses `id' pairs */
            memcpy(key + o, "<id>", 4);
            o += 4;
            p++;
            continue;
        }
        if (c >= '0' && c <= '9') { p++; continue; }
        key[o++] = c;
        p++;
    }
    key[o] = '\0';
}

#define FOLD_MAX_GROUPS 8

typedef struct {
    char key[256];
    char first[512];
    int  count;
} fold_group_t;

static int fold_stderr(const char* tmppath, const char* toolpath,
                       jash_writer_t w, void* ctx) {
    FILE* f = fopen(tmppath, "r");
    if (!f) return 0;

    fold_group_t g[FOLD_MAX_GROUPS];
    int ng = 0, extra_groups = 0;
    long n_errors = 0, n_warnings = 0, n_other = 0;

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        size_t l = strlen(line);
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = '\0';
        if (!l) continue;
        if (is_error_line(line)) {
            n_errors++;
            char key[256];
            fold_key(line, key, sizeof(key));
            int found = 0;
            for (int i = 0; i < ng; i++)
                if (strcmp(g[i].key, key) == 0) { g[i].count++; found = 1; break; }
            if (!found) {
                if (ng < FOLD_MAX_GROUPS) {
                    snprintf(g[ng].key, sizeof(g[ng].key), "%s", key);
                    snprintf(g[ng].first, sizeof(g[ng].first), "%s", line);
                    g[ng].count = 1;
                    ng++;
                } else {
                    extra_groups++;
                }
            }
        } else if (is_warning_line(line)) {
            n_warnings++;
        } else {
            n_other++;
        }
    }
    fclose(f);

    if (n_errors == 0) {
        /* Nothing to fold: warnings/noise only. Say one line if there was
         * anything at all; the full file still moves to overflow below
         * ONLY when we folded, so re-emit nothing here and fall back. */
        return 0;
    }

    /* Preserve the FULL stderr as a recall-able overflow file (rename —
     * zero copy; the caller's unlink then no-ops). */
    char handle[256] = "";
    {
        const char* home = getenv("HOME");
        if (home && *home) {
            char dir[512];
            snprintf(dir, sizeof(dir), "%s/.jash/overflow", home);
            mkdir(dir, 0700);
            const char* base = toolpath ? path_basename(toolpath) : "tool";
            char dst[768];
            snprintf(dst, sizeof(dst), "%s/%d-%s-stderr.txt",
                     dir, (int)getpid(), base);
            if (rename(tmppath, dst) == 0) {
                const char* h = dst;
                for (const char* p = dst; *p; p++) if (*p == '/') h = p + 1;
                snprintf(handle, sizeof(handle), "%s", h);
            }
        }
    }

    /* The fold REPLACES the stderr stream, so it must live on fd 2 —
     * stdout stays command-pure, and anything watching stderr for errors
     * still finds them. (The legacy path also re-emits raw lines on fd 2.) */
    (void)w; (void)ctx;
    for (int i = 0; i < ng; i++) {
        dprintf(2, "%s\n", g[i].first);
        if (g[i].count > 1)
            dprintf(2, "  … +%d more with the same cause\n", g[i].count - 1);
        const char* h = err_fix_hint(g[i].first);
        if (h) dprintf(2, "  [fix?] %s\n", h);
    }

    dprintf(2,
        "[jash err-fold] %d root cause%s (%ld error line%s), %ld warning%s, "
        "%ld other line%s suppressed%s%s\n",
        ng + extra_groups, (ng + extra_groups) == 1 ? "" : "s",
        n_errors, n_errors == 1 ? "" : "s",
        n_warnings, n_warnings == 1 ? "" : "s",
        n_other, n_other == 1 ? "" : "s",
        handle[0] ? " · full stderr: recall " : "",
        handle[0] ? handle : "");
    return 1;
}

void jash_err_digest_run2(const char* tmppath, const char* toolpath,
                          jash_writer_t w, void* ctx) {
    const char* fold_env = getenv("JASH_ERR_FOLD");
    int fold_on = !(fold_env && fold_env[0] == '0') &&
                  jash_env_smells_like_agent();
    if (fold_on && fold_stderr(tmppath, toolpath, w, ctx))
        return;
    jash_err_digest_run(tmppath, w, ctx);
}

void jash_err_digest_run(const char* tmppath, jash_writer_t w, void* ctx) {
    if (!tmppath || !w) return;

    int fd = open(tmppath, O_RDONLY);
    if (fd < 0) return;

    char digest[DIGEST_MAX][DIGEST_LINE_MAX];
    int n_errors = 0;

    char line[1024];
    int  lpos = 0;
    char rbuf[4096];
    ssize_t nr;

    while ((nr = read(fd, rbuf, sizeof(rbuf))) > 0) {
        for (ssize_t i = 0; i < nr; i++) {
            char c = rbuf[i];
            if (c == '\n' || lpos >= (int)sizeof(line) - 1) {
                line[lpos] = '\0';
                { ssize_t nw __attribute__((unused));
                  nw = write(2, line, (size_t)lpos);
                  nw = write(2, "\n", 1); }
                if (n_errors < DIGEST_MAX && lpos > 0 && is_error_line(line)) {
                    size_t cplen = (size_t)lpos < DIGEST_LINE_MAX - 1
                                   ? (size_t)lpos : DIGEST_LINE_MAX - 1;
                    memcpy(digest[n_errors], line, cplen);
                    digest[n_errors][cplen] = '\0';
                    n_errors++;
                }
                lpos = 0;
            } else {
                line[lpos++] = c;
            }
        }
    }
    // Flush partial last line (no trailing newline)
    if (lpos > 0) {
        line[lpos] = '\0';
        { ssize_t nw __attribute__((unused));
          nw = write(2, line, (size_t)lpos);
          nw = write(2, "\n", 1); }
        if (n_errors < DIGEST_MAX && is_error_line(line)) {
            size_t cplen = (size_t)lpos < DIGEST_LINE_MAX - 1
                           ? (size_t)lpos : DIGEST_LINE_MAX - 1;
            memcpy(digest[n_errors], line, cplen);
            digest[n_errors][cplen] = '\0';
            n_errors++;
        }
    }
    close(fd);

    if (n_errors == 0) return;

    char hdr[64];
    snprintf(hdr, sizeof(hdr), "%d error%s", n_errors, n_errors == 1 ? "" : "s");
    jash_writes(w, ctx, "\033[90m[err-digest]\033[0m \033[91m");
    jash_writes(w, ctx, hdr);
    jash_writes(w, ctx, ":\033[0m\n");
    for (int i = 0; i < n_errors; i++) {
        jash_writes(w, ctx, "  \033[33m");
        jash_writes(w, ctx, digest[i]);
        jash_writes(w, ctx, "\033[0m\n");
    }

    /* F12: surface fix hints for recognized signatures (deduped). */
    const char* hints[DIGEST_MAX];
    int nh = 0;
    for (int i = 0; i < n_errors; i++) {
        const char* h = err_fix_hint(digest[i]);
        if (!h) continue;
        int dup = 0;
        for (int j = 0; j < nh; j++) if (hints[j] == h) { dup = 1; break; }
        if (!dup && nh < DIGEST_MAX) hints[nh++] = h;
    }
    if (nh > 0) {
        jash_writes(w, ctx, "\033[90m[fix?]\033[0m\n");
        for (int j = 0; j < nh; j++) {
            jash_writes(w, ctx, "  \033[36m- ");
            jash_writes(w, ctx, hints[j]);
            jash_writes(w, ctx, "\033[0m\n");
        }
    }
}
