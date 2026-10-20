// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — core/drift — environment-drift forensics (ADR-0043 W1).
// See drift.h for the contract. F E R M I ∞ H A R T <contact@fermihart.com>
#define _GNU_SOURCE
#include "drift.h"
#include "memo.h"    /* jash_env_smells_like_agent */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <time.h>
#include <limits.h>

#define DRIFT_ROWS_MAX 256
#define DRIFT_CMD_KEEP 80
#define DRIFT_TOOL_MAX 160
#define DRIFT_LINE_MAX 640

/* Build-relevant env vars fingerprinted individually (4 hex each, 16 vars). */
static const char* const drift_envs[16] = {
    "CC","CXX","CFLAGS","CXXFLAGS","LDFLAGS","MAKEFLAGS","LD_LIBRARY_PATH",
    "VIRTUAL_ENV","CONDA_PREFIX","NODE_ENV","PYTHONPATH","GOPATH","JAVA_HOME",
    "RUSTFLAGS","LANG","LC_ALL"
};

static unsigned long long drift_fnv64(const char* s, unsigned long long h) {
    for (; s && *s; s++) { h ^= (unsigned char)*s; h *= 1099511628211ULL; }
    return h;
}
static unsigned drift_fnv32(const char* s) {
    unsigned h = 2166136261u;
    for (; s && *s; s++) { h ^= (unsigned char)*s; h *= 16777619u; }
    return h;
}

typedef struct {
    char key[17];                /* fnv64(cmd \1 cwd) */
    char path_h[17];             /* fnv64(PATH) */
    char tool[DRIFT_TOOL_MAX];   /* resolved argv0, or "?" */
    long tool_mt, tool_sz;       /* tool binary mtime (s) + size */
    char head[13];               /* git HEAD (12 hex) or "?" */
    char env[65];                /* 16 × 4-hex per-var hashes; "----" = unset */
    long ts;                     /* row timestamp (last success) */
} drift_fp_t;

/* First command word = the tool. Skips leading VAR=val assignments and `env`. */
static void drift_tool_word(const char* cmd, char* out, size_t cap) {
    out[0] = '\0';
    const char* p = cmd;
    for (int hop = 0; hop < 8; hop++) {
        while (*p == ' ' || *p == '\t') p++;
        const char* e = p;
        while (*e && *e != ' ' && *e != '\t' && *e != ';' && *e != '|' &&
               *e != '&' && *e != '\n') e++;
        if (e == p) return;
        size_t n = (size_t)(e - p);
        int is_assign = 0;
        for (const char* q = p; q < e; q++)
            if (*q == '=') { is_assign = 1; break; }
        if (is_assign || (n == 3 && strncmp(p, "env", 3) == 0)) { p = e; continue; }
        if (n >= cap) n = cap - 1;
        memcpy(out, p, n); out[n] = '\0';
        return;
    }
}

/* Resolve the tool via PATH (unless it already has a slash); stat it. */
static void drift_resolve_tool(const char* cmd, char* out, size_t cap,
                               long* mt, long* sz) {
    char word[256];
    drift_tool_word(cmd, word, sizeof word);
    snprintf(out, cap, "?"); *mt = 0; *sz = 0;
    if (!word[0]) return;
    char cand[PATH_MAX];
    struct stat st;
    if (strchr(word, '/')) {
        if (stat(word, &st) == 0) {
            snprintf(out, cap, "%s", word);
            *mt = (long)st.st_mtime; *sz = (long)st.st_size;
        }
        return;
    }
    const char* path = getenv("PATH");
    if (!path) return;
    const char* s = path;
    while (*s) {
        const char* e = strchr(s, ':');
        size_t dn = e ? (size_t)(e - s) : strlen(s);
        if (dn > 0 && dn < sizeof(cand) - strlen(word) - 2) {
            memcpy(cand, s, dn);
            snprintf(cand + dn, sizeof(cand) - dn, "/%s", word);
            if (stat(cand, &st) == 0 && access(cand, X_OK) == 0) {
                snprintf(out, cap, "%s", cand);
                *mt = (long)st.st_mtime; *sz = (long)st.st_size;
                return;
            }
        }
        if (!e) break;
        s = e + 1;
    }
}

/* git HEAD: walk up from cwd looking for .git/HEAD; deref one level of ref.
 * JINV-057: publico. `resume` precisa da MESMA nocao de HEAD que o drift —
 * duas implementacoes da mesma coisa foi o pior achado desta auditoria
 * (JINV-015/017), entao esta helper deixou de ser static em vez de ser copiada. */
void jash_drift_git_head(const char* cwd, char out[13]) {
    snprintf(out, 13, "?");
    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%s", cwd);
    for (int up = 0; up < 6; up++) {
        char hp[PATH_MAX + 32];
        snprintf(hp, sizeof hp, "%s/.git/HEAD", dir);
        FILE* f = fopen(hp, "r");
        if (f) {
            char buf[300] = "";
            if (!fgets(buf, sizeof buf, f)) buf[0] = '\0';
            fclose(f);
            buf[strcspn(buf, "\r\n")] = '\0';
            if (strncmp(buf, "ref: ", 5) == 0) {
                char rp[PATH_MAX + 320];
                snprintf(rp, sizeof rp, "%s/.git/%s", dir, buf + 5);
                FILE* rf = fopen(rp, "r");
                char sha[64] = "";
                if (rf) {
                    if (!fgets(sha, sizeof sha, rf)) sha[0] = '\0';
                    fclose(rf);
                }
                sha[strcspn(sha, "\r\n")] = '\0';
                if (strlen(sha) >= 12) { memcpy(out, sha, 12); out[12] = '\0'; }
                else snprintf(out, 13, "unborn");
            } else if (strlen(buf) >= 12) {
                memcpy(out, buf, 12); out[12] = '\0';
            }
            return;
        }
        char* slash = strrchr(dir, '/');
        if (!slash || slash == dir) return;
        *slash = '\0';
    }
}

static void drift_env_hex(char out[65]) {
    int o = 0;
    for (int i = 0; i < 16; i++) {
        const char* v = getenv(drift_envs[i]);
        if (!v) { memcpy(out + o, "----", 4); }
        else    { snprintf(out + o, 5, "%04x", drift_fnv32(v) & 0xffff); }
        o += 4;
    }
    out[64] = '\0';
}

void jash_drift_command_key(const char* cmd, const char* cwd, char out[17]) {
    unsigned long long k = drift_fnv64(cmd, 1469598103934665603ULL);
    k = drift_fnv64("\1", k);
    k = drift_fnv64(cwd, k);
    snprintf(out, 17, "%016llx", k);
}

static void drift_fingerprint(const char* cmd, const char* cwd, drift_fp_t* fp) {
    memset(fp, 0, sizeof *fp);
    jash_drift_command_key(cmd, cwd, fp->key);
    snprintf(fp->path_h, sizeof fp->path_h, "%016llx",
             drift_fnv64(getenv("PATH") ? getenv("PATH") : "", 1469598103934665603ULL));
    drift_resolve_tool(cmd, fp->tool, sizeof fp->tool, &fp->tool_mt, &fp->tool_sz);
    jash_drift_git_head(cwd, fp->head);
    drift_env_hex(fp->env);
    fp->ts = (long)time(NULL);
}

int jash_drift_enabled(void) {
    const char* v = getenv("JASH_DRIFT");
    if (v && v[0] == '0') return 0;
    if (!getenv("HOME")) return 0;
    return jash_env_smells_like_agent();
}

static int drift_ledger_path(char* out, size_t cap) {
    const char* home = getenv("HOME");
    if (!home || !*home) return 0;
    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%s/.jash", home);
    mkdir(dir, 0700);
    snprintf(out, cap, "%s/.jash/drift.tsv", home);
    return 1;
}

/* One sanitized copy of the command for the ledger row / warning line. */
static void drift_cmd_keep(const char* cmd, char* out, size_t cap) {
    size_t o = 0;
    for (const char* p = cmd; *p && o < cap - 1 && o < DRIFT_CMD_KEEP; p++)
        out[o++] = (*p == '\t' || *p == '\n') ? ' ' : *p;
    out[o] = '\0';
}

static const char* drift_age(long secs, char* buf, size_t cap) {
    if (secs < 0) secs = 0;
    if (secs < 120)        snprintf(buf, cap, "%lds", secs);
    else if (secs < 7200)  snprintf(buf, cap, "%ldm", secs / 60);
    else if (secs < 172800) snprintf(buf, cap, "%ldh", secs / 3600);
    else                   snprintf(buf, cap, "%ldd", secs / 86400);
    return buf;
}

void jash_drift_note(const char* cmd, int rc, const char* cwd) {
    if (!cmd || !*cmd || !jash_drift_enabled()) return;
    char cwdbuf[PATH_MAX];
    if (!cwd) {
        if (!getcwd(cwdbuf, sizeof cwdbuf)) return;
        cwd = cwdbuf;
    }
    char path[PATH_MAX];
    if (!drift_ledger_path(path, sizeof path)) return;

    drift_fp_t live;
    drift_fingerprint(cmd, cwd, &live);

    int fd = open(path, O_RDWR | O_CREAT, 0600);
    if (fd < 0) return;
    if (flock(fd, LOCK_EX) != 0) { close(fd); return; }
    FILE* f = fdopen(fd, "r+");
    if (!f) { close(fd); return; }

    /* Load rows (heap — never big static BSS; see BUG-065). */
    char (*rows)[DRIFT_LINE_MAX] = calloc(DRIFT_ROWS_MAX, DRIFT_LINE_MAX);
    if (!rows) { fclose(f); return; }
    int nrows = 0, hit = -1;
    long oldest_ts = LONG_MAX; int oldest_i = -1;
    char line[DRIFT_LINE_MAX];
    while (nrows < DRIFT_ROWS_MAX && fgets(line, sizeof line, f)) {
        if (strncmp(line, live.key, 16) == 0 && line[16] == '\t') hit = nrows;
        long rts = 0; sscanf(line, "%*16s\t%ld", &rts);
        if (rts < oldest_ts) { oldest_ts = rts; oldest_i = nrows; }
        snprintf(rows[nrows], DRIFT_LINE_MAX, "%s", line);
        nrows++;
    }

    if (rc == 0) {
        /* Upsert this key's fingerprint row. */
        char cmd80[DRIFT_CMD_KEEP + 1];
        drift_cmd_keep(cmd, cmd80, sizeof cmd80);
        char row[DRIFT_LINE_MAX];
        snprintf(row, sizeof row, "%s\t%ld\t%s\t%s\t%ld\t%ld\t%s\t%s\t%s\n",
                 live.key, live.ts, live.path_h, live.tool, live.tool_mt,
                 live.tool_sz, live.head, live.env, cmd80);
        if (hit >= 0) {
            /* The row embeds ts (last success), which advances on every run,
             * so a full-row compare almost never hits and the whole ledger
             * got rewritten per command. Compare only the fingerprint part
             * (everything after "key\t ts\t"): when nothing but the timestamp
             * moved, the ledger needs no rewrite and the stored ts stays. */
            const char* old_fp = strchr(rows[hit], '\t');
            const char* new_fp = strchr(row, '\t');
            if (old_fp) old_fp = strchr(old_fp + 1, '\t');
            if (new_fp) new_fp = strchr(new_fp + 1, '\t');
            if (old_fp && new_fp && strcmp(old_fp, new_fp) == 0) {
                free(rows); fclose(f); return; /* fingerprint unchanged */
            }
            snprintf(rows[hit], DRIFT_LINE_MAX, "%s", row);
        } else if (nrows < DRIFT_ROWS_MAX) {
            snprintf(rows[nrows++], DRIFT_LINE_MAX, "%s", row);
        } else if (oldest_i >= 0) {
            snprintf(rows[oldest_i], DRIFT_LINE_MAX, "%s", row);
        }
        rewind(f);
        int tfd = fileno(f);
        if (ftruncate(tfd, 0) == 0) {
            for (int i = 0; i < nrows; i++) fputs(rows[i], f);
        }
        free(rows); fclose(f);
        return;
    }

    /* Failure path: nothing recorded → nothing to say. */
    if (hit < 0) { free(rows); fclose(f); return; }

    /* Parse the stored row. */
    drift_fp_t old; memset(&old, 0, sizeof old);
    {
        char* save = NULL;
        char* fields[9] = {0};
        int nf = 0;
        for (char* t = strtok_r(rows[hit], "\t\n", &save);
             t && nf < 9; t = strtok_r(NULL, "\t\n", &save))
            fields[nf++] = t;
        if (nf < 8) { free(rows); fclose(f); return; }
        snprintf(old.key, sizeof old.key, "%s", fields[0]);
        old.ts = atol(fields[1]);
        snprintf(old.path_h, sizeof old.path_h, "%s", fields[2]);
        snprintf(old.tool, sizeof old.tool, "%s", fields[3]);
        old.tool_mt = atol(fields[4]);
        old.tool_sz = atol(fields[5]);
        snprintf(old.head, sizeof old.head, "%s", fields[6]);
        snprintf(old.env, sizeof old.env, "%s", fields[7]);
    }
    free(rows);
    fclose(f);   /* releases the flock */

    /* Diff live vs last success. */
    char diffs[400] = "";
    size_t o = 0;
    #define DADD(...) do { if (o < sizeof(diffs) - 2) { \
        if (o) o += (size_t)snprintf(diffs + o, sizeof(diffs) - o, "; "); \
        o += (size_t)snprintf(diffs + o, sizeof(diffs) - o, __VA_ARGS__); \
        if (o >= sizeof(diffs)) o = sizeof(diffs) - 1; } } while (0)
    if (strcmp(old.path_h, live.path_h) != 0) DADD("PATH");
    if (strcmp(old.tool, live.tool) != 0)
        DADD("tool %s -> %s", old.tool, live.tool);
    else if (old.tool_mt != live.tool_mt || old.tool_sz != live.tool_sz)
        DADD("tool %s rebuilt", live.tool);
    if (strcmp(old.head, live.head) != 0)
        DADD("git HEAD %s -> %s", old.head, live.head);
    if (strlen(old.env) == 64 && strlen(live.env) == 64) {
        for (int i = 0; i < 16; i++) {
            if (strncmp(old.env + i * 4, live.env + i * 4, 4) == 0) continue;
            int was_unset = strncmp(old.env + i * 4, "----", 4) == 0;
            int is_unset  = strncmp(live.env + i * 4, "----", 4) == 0;
            DADD("%s%s", is_unset ? "-" : (was_unset ? "+" : ""), drift_envs[i]);
        }
    }
    #undef DADD
    if (!diffs[0]) return;   /* environment identical — failure isn't drift */

    char cmd80[DRIFT_CMD_KEEP + 1];
    drift_cmd_keep(cmd, cmd80, sizeof cmd80);
    char agebuf[24];
    fprintf(stderr,
        "[jash drift] `%s` last succeeded %s ago; changed since: %s "
        "(JASH_DRIFT=0 silences)\n",
        cmd80, drift_age(live.ts - old.ts, agebuf, sizeof agebuf), diffs);
}
