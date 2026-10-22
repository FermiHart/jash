// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Builtins: ulimit, umask, times, command

#include "builtin.h"
#include "shell_output.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/times.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <math.h>
#include <function_defs.h>
#include <parser/exec.h>
#include <theme/io.h>

/* ── ulimit ──────────────────────────────────────────────────────────────── */

typedef struct {
    char        flag;
    int         resource;
    const char* label;
    long        scale;   /* kernel unit → ulimit display unit (divide) */
} ulim_res_t;

static const ulim_res_t ulim_table[] = {
    { 'c', RLIMIT_CORE,   "core file size (blocks)", 512 },
    { 'd', RLIMIT_DATA,   "data seg size (kbytes)",  1024 },
    { 'f', RLIMIT_FSIZE,  "file size (blocks)",      512 },
#ifdef RLIMIT_MEMLOCK
    { 'l', RLIMIT_MEMLOCK,"locked memory (kbytes)",  1024 },
#endif
#ifdef RLIMIT_RSS
    { 'm', RLIMIT_RSS,    "max memory size (kbytes)",1024 },
#endif
    { 'n', RLIMIT_NOFILE, "open files",              1 },
    { 's', RLIMIT_STACK,  "stack size (kbytes)",     1024 },
    { 't', RLIMIT_CPU,    "cpu time (seconds)",      1 },
    { 'u', RLIMIT_NPROC,  "max user processes",      1 },
#ifdef RLIMIT_AS
    { 'v', RLIMIT_AS,     "virtual memory (kbytes)", 1024 },
#endif
    { 0, 0, NULL, 0 }
};

static const ulim_res_t* ulim_find(char flag) {
    for (int i = 0; ulim_table[i].flag; i++)
        if (ulim_table[i].flag == flag) return &ulim_table[i];
    return NULL;
}

static void ulim_print(jash_writer_t w, void* ctx, rlim_t val, long scale) {
    if (val == RLIM_INFINITY)
        jash_writes(w, ctx, "unlimited\n");
    else {
        char buf[32];
        snprintf(buf, sizeof buf, "%llu\n", (unsigned long long)(val / (rlim_t)scale));
        jash_writes(w, ctx, buf);
    }
}

int jash_b_ulimit(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    int structured = jash_shell_state_try(s, w, ctx, "ulimit", args);
    if (structured != JASH_SHELL_TEXT) return structured;
    (void)s;
    if (!args) args = "";
    while (*args == ' ' || *args == '\t') args++;

    int hard = 0, soft = 1;   /* default: soft limit */
    int show_all = 0;
    char res_flag = 'f';       /* bash default: file size */
    const char* set_val = NULL;

    /* Parse flags */
    const char* p = args;
    while (*p == '-') {
        p++;
        char c = *p++;
        if (c == 'H') { hard = 1; soft = 0; }
        else if (c == 'S') { soft = 1; hard = 0; }
        else if (c == 'a') { show_all = 1; }
        else if (ulim_find(c)) { res_flag = c; }
        else {
            char msg[64];
            snprintf(msg, sizeof msg, "ulimit: invalid option: -%c\n", c);
            jash_writes(w, ctx, msg);
            return 1;
        }
        while (*p == ' ' || *p == '\t') p++;
    }
    if (*p) set_val = p;

    if (show_all) {
        for (int i = 0; ulim_table[i].flag; i++) {
            struct rlimit rl;
            if (getrlimit(ulim_table[i].resource, &rl) != 0) continue;
            char line[80];
            snprintf(line, sizeof line, "%-32s (-%c)  ", ulim_table[i].label, ulim_table[i].flag);
            jash_writes(w, ctx, line);
            ulim_print(w, ctx, soft ? rl.rlim_cur : rl.rlim_max, ulim_table[i].scale);
        }
        return 0;
    }

    const ulim_res_t* r = ulim_find(res_flag);
    if (!r) { jash_writes(w, ctx, "ulimit: unsupported resource\n"); return 1; }

    struct rlimit rl;
    if (getrlimit(r->resource, &rl) != 0) {
        jash_writes(w, ctx, "ulimit: getrlimit failed\n");
        return 1;
    }

    if (!set_val) {
        ulim_print(w, ctx, soft ? rl.rlim_cur : rl.rlim_max, r->scale);
        return 0;
    }

    rlim_t new_val;
    if (strcmp(set_val, "unlimited") == 0)
        new_val = RLIM_INFINITY;
    else {
        char* end;
        unsigned long long v = strtoull(set_val, &end, 10);
        if (end == set_val) { jash_writes(w, ctx, "ulimit: invalid value\n"); return 1; }
        new_val = (rlim_t)(v * (unsigned long long)r->scale);
    }
    if (soft)  rl.rlim_cur = new_val;
    if (hard)  rl.rlim_max = new_val;
    if (setrlimit(r->resource, &rl) != 0) {
        jash_writes(w, ctx, "ulimit: cannot set limit\n");
        return 1;
    }
    return 0;
}

/* ── umask ───────────────────────────────────────────────────────────────── */

int jash_b_umask(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    int structured = jash_shell_state_try(s, w, ctx, "umask", args);
    if (structured != JASH_SHELL_TEXT) return structured;
    (void)s;
    if (!args) args = "";
    while (*args == ' ' || *args == '\t') args++;

    /* -S flag: symbolic output */
    int symbolic = 0;
    if (args[0] == '-' && args[1] == 'S') {
        symbolic = 1;
        args += 2;
        while (*args == ' ' || *args == '\t') args++;
    }

    if (!*args) {
        /* Read and restore — umask() is get+set combined */
        mode_t m = umask(0);
        umask(m);
        if (symbolic) {
            char buf[32];
            snprintf(buf, sizeof buf, "u=%s%s%s,g=%s%s%s,o=%s%s%s\n",
                     (m & S_IRUSR) ? "" : "r",
                     (m & S_IWUSR) ? "" : "w",
                     (m & S_IXUSR) ? "" : "x",
                     (m & S_IRGRP) ? "" : "r",
                     (m & S_IWGRP) ? "" : "w",
                     (m & S_IXGRP) ? "" : "x",
                     (m & S_IROTH) ? "" : "r",
                     (m & S_IWOTH) ? "" : "w",
                     (m & S_IXOTH) ? "" : "x");
            jash_writes(w, ctx, buf);
        } else {
            char buf[16];
            snprintf(buf, sizeof buf, "%04o\n", (unsigned)m);
            jash_writes(w, ctx, buf);
        }
        return 0;
    }

    /* Set new mask (octal) */
    char* end;
    unsigned long v = strtoul(args, &end, 8);
    if (end == args || *end) {
        jash_writes(w, ctx, "umask: invalid mask\n");
        return 1;
    }
    umask((mode_t)(v & 0777));
    return 0;
}

/* ── times ───────────────────────────────────────────────────────────────── */

int jash_b_times(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    int structured = jash_shell_state_try(s, w, ctx, "times", args);
    if (structured != JASH_SHELL_TEXT) return structured;
    (void)s; (void)args;
    struct tms t;
    long hz = sysconf(_SC_CLK_TCK);
    if (hz <= 0) {
        jash_writes(w, ctx, "times: clock tick rate unavailable\n");
        return 1;
    }
    if (times(&t) == (clock_t)-1) {
        jash_writes(w, ctx, "times: CPU time unavailable\n");
        return 1;
    }

    double su = (double)t.tms_utime  / hz;
    double ss = (double)t.tms_stime  / hz;
    double cu = (double)t.tms_cutime / hz;
    double cs = (double)t.tms_cstime / hz;

    char line[128];
    snprintf(line, sizeof line, "%ldm%.3fs %ldm%.3fs\n%ldm%.3fs %ldm%.3fs\n",
             (long)(su / 60), fmod(su, 60.0),
             (long)(ss / 60), fmod(ss, 60.0),
             (long)(cu / 60), fmod(cu, 60.0),
             (long)(cs / 60), fmod(cs, 60.0));
    jash_writes(w, ctx, line);
    return 0;
}

/* ── command ─────────────────────────────────────────────────────────────── */

int jash_b_command(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    if (!args || !*args) return 0;
    while (*args == ' ' || *args == '\t') args++;

    if (args[0] == '-' && (args[1] == 'v' || args[1] == 'V') &&
        (args[2] == ' ' || args[2] == '\t')) {
        int verbose = (args[1] == 'V');
        args += 2;
        while (*args == ' ' || *args == '\t') args++;

        char name[256];
        unsigned ni = 0;
        while (args[ni] && args[ni] != ' ' && args[ni] != '\t' &&
               ni + 1 < sizeof(name)) { name[ni] = args[ni]; ni++; }
        name[ni] = '\0';
        if (!name[0]) return 1;

        if (s->functions && jash_function_exists(s->functions, name)) {
            if (verbose) jash_writef(w, ctx, "%s is a function\n", name);
            else         jash_writef(w, ctx, "%s\n", name);
            return 0;
        }
        extern const jash_builtin_t* jash_builtin_lookup(const char*);
        const jash_builtin_t* b = jash_builtin_lookup(name);
        if (b) {
            if (verbose) jash_writef(w, ctx, "%s is a shell builtin\n", name);
            else         jash_writef(w, ctx, "%s\n", name);
            return 0;
        }
        if (name[0] == '/' || (name[0] == '.' && (name[1] == '/' ||
            (name[1] == '.' && name[2] == '/')))) {
            if (access(name, X_OK) == 0) { jash_writef(w, ctx, "%s\n", name); return 0; }
            return 1;
        }
        char fullpath[512];
        if (jash_path_lookup(s, name, fullpath, sizeof fullpath)) {
            if (verbose) jash_writef(w, ctx, "%s is %s\n", name, fullpath);
            else         jash_writef(w, ctx, "%s\n", fullpath);
            return 0;
        }
        if (verbose) jash_writef(jash_stderr_writer, 0, "%s: not found\n", name);
        return 1;
    }

    /* BUG-013: bypass any shadowing function for this one command — run the
     * builtin/external named by `args`. Without this, `echo(){ command echo …; }`
     * re-entered the function forever (max recursion depth). */
    extern int jash_eval_line(jash_state_t*, jash_writer_t, void*, const char*);
    if (s) s->skip_function_lookup = 1;
    return jash_eval_line(s, w, ctx, args);
}
