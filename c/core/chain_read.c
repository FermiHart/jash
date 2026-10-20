// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — core/chain_read — shared chain.log reader. F E R M I ∞ H A R T
#define _GNU_SOURCE
#include "chain_read.h"
#include "adopt.h"
#include <stdlib.h>
#include <string.h>

FILE* jash_chain_open(void) {
    const char* home = getenv("HOME");
    if (!home || !*home) return NULL;
    char path[600];
    /* ADR-0051 W6: per-repo chain — mirror the writer (sigil.c open_chain_log).
     * Inside a repo the chain lives in chain-<key>.log; the historic global
     * chain.log is only written outside one. Without this, every chain_read
     * consumer (distill, blame-file, bg-jobs attestation, loopguard) went
     * blind inside repos the moment W6 landed. */
    const char* k = jash_repo_key();
    if (k && k[0] != '-')
        snprintf(path, sizeof(path), "%s/.jash/chain-%s.log", home, k);
    else
        snprintf(path, sizeof(path), "%s/.jash/chain.log", home);
    return fopen(path, "r");
}

static void parse_tag(const char* line, char* tag, size_t cap) {
    const char* tp = strstr(line, " tag=");
    if (!tp) { snprintf(tag, cap, "-"); return; }
    tp += 5;
    size_t i = 0;
    while (*tp && *tp != ' ' && *tp != '\t' && *tp != '\n' && i < cap - 1)
        tag[i++] = *tp++;
    tag[i] = '\0';
}

/* Parse one already-read chain line into `e`. Returns 1 on success, 0 if the
 * line is not a usable entry (malformed, or a SESSION_END marker). No field of
 * `e` is written unless the line parses, so a caller may pass its destination
 * slot directly. */
static int parse_chain_line(const char* line, jash_chain_entry_t* e) {
    unsigned long long seq = 0, ts_sec = 0;
    unsigned long ts_ns = 0;
    char hash[CHREAD_HASH_MAX] = {0};
    if (sscanf(line, "%llu %llu.%lu %79s", &seq, &ts_sec, &ts_ns, hash) < 3)
        return 0;
    const char* cp = strstr(line, " cmd=");
    if (!cp) return 0;
    const char* cmd = cp + 5;
    if (strncmp(cmd, "SESSION_END", 11) == 0) return 0;
    int rc = 0;
    const char* rp = strstr(line, " rc=");
    if (rp) sscanf(rp, " rc=%d", &rc);
    e->seq = seq;
    e->ts_sec = ts_sec;
    e->ts_ns = ts_ns;
    e->rc = rc;
    snprintf(e->hash, sizeof(e->hash), "%s", hash);
    parse_tag(line, e->tag, sizeof(e->tag));
    snprintf(e->cmd, sizeof(e->cmd), "%s", cmd);
    /* strip trailing newline */
    size_t len = strlen(e->cmd);
    if (len > 0 && e->cmd[len - 1] == '\n') e->cmd[len - 1] = '\0';
    return 1;
}

int jash_chain_next(FILE* f, jash_chain_entry_t* e) {
    char line[2048];
    while (fgets(line, sizeof(line), f))
        if (parse_chain_line(line, e)) return 1;
    return 0;
}

/* Reverse buf[lo..hi] in place (empty/singleton ranges are no-ops). */
static void rev_range(jash_chain_entry_t* b, int lo, int hi) {
    while (lo < hi) {
        jash_chain_entry_t t = b[lo];
        b[lo] = b[hi];
        b[hi] = t;
        lo++;
        hi--;
    }
}

int jash_chain_tail(jash_chain_entry_t* buf, int cap, int n) {
    if (!buf || cap <= 0) return 0;
    int want = cap;
    if (want > 256) want = 256;
    if (n > 0 && n < want) want = n;

    FILE* f = jash_chain_open();
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    long fsize = ftell(f);
    if (fsize <= 0) { fclose(f); return 0; }

    /* JINV-102: bounded backward window, not a full forward pass.
     *
     * The old implementation ("single forward pass with ring buffer") parsed
     * EVERY entry in the chain just to return the last 32. loopguard calls
     * this on every command, so a long-lived install (105MB / ~400k entries)
     * paid a full 105MB parse per command: ~4s startup, growing without
     * bound. Read a window back from EOF instead, and grow it only if it did
     * not contain enough valid entries. Startup is now O(window), not
     * O(chain size). Guarded by tests/test_chain_tail.sh. */
    long win = 256L * 1024L;
    const long WIN_MAX = 16L * 1024L * 1024L;
    int filled = 0;

    for (;;) {
        if (win > fsize) win = fsize;
        long start = fsize - win;
        char* mem = (char*)malloc((size_t)win + 1);
        if (!mem) break;
        if (fseek(f, start, SEEK_SET) != 0) { free(mem); break; }
        size_t got = fread(mem, 1, (size_t)win, f);
        mem[got] = '\0';

        char* p = mem;
        if (start > 0) {
            /* The window almost certainly begins mid-line; drop that partial
             * line so we never parse a truncated entry. */
            char* nl = (char*)memchr(mem, '\n', got);
            if (!nl) {
                free(mem);
                if (win >= fsize || win >= WIN_MAX) break;
                win *= 4;
                continue;
            }
            p = nl + 1;
        }

        int total = 0, ring_pos = 0;
        while (*p) {
            char* nl = strchr(p, '\n');
            if (nl) *nl = '\0';
            if (*p && parse_chain_line(p, &buf[ring_pos])) {
                ring_pos = (ring_pos + 1) % want;
                total++;
            }
            if (!nl) break;
            p = nl + 1;
        }
        free(mem);

        filled = (total < want) ? total : want;
        /* Short by fewer entries than asked for, but more file remains before
         * the window? Widen and retry; otherwise this is all there is. */
        if (filled >= want || start == 0 || win >= fsize || win >= WIN_MAX) {
            /* buf now holds arrival order rotated by ring_pos. Newest-first =
             * reverse of (buf[ring_pos..] ++ buf[..ring_pos-1]), which is
             * exactly these two in-place segment reversals. */
            rev_range(buf, 0, ring_pos - 1);
            rev_range(buf, ring_pos, filled - 1);
            break;
        }
        win *= 4;
    }
    fclose(f);
    return filled;
}

int jash_chain_tokenize(const char* cmd, char** argv, int max_argv) {
    int argc = 0;
    const char* p = cmd;
    while (*p && argc < max_argv) {
        /* skip whitespace */
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        char quote = 0;
        if (*p == '\'' || *p == '"') { quote = *p; p++; }
        argv[argc] = (char*)p;
        while (*p) {
            if (quote) {
                if (*p == quote) { p++; break; }
            } else {
                if (*p == ' ' || *p == '\t') break;
            }
            p++;
        }
        argc++;
    }
    return argc;
}

/* jash_chain_tokenize_nul - like tokenize but NUL-terminates each token.
 * Uses caller-provided buffer for NUL termination (preserves original string).
 * This is needed for jaccard similarity comparison (BUG-152). */
int jash_chain_tokenize_nul(const char* cmd, char* work_buf, size_t buf_cap, char** argv, int max_argv) {
    if (!cmd || !work_buf || !buf_cap || !argv || max_argv < 0) return -1;
    int argc = 0; size_t used = 0;
    const char* src = cmd;
    work_buf[0] = 0;
    while (*src) {
        while (*src == ' ' || *src == '\t') src++;
        if (!*src) break;
        if (argc == max_argv || used == buf_cap) return -1;
        char quote = 0;
        if (*src == '\'' || *src == '"') { quote = *src; src++; }
        argv[argc] = work_buf + used;
        while (*src && (quote ? *src != quote : *src != ' ' && *src != '\t')) {
            if (used + 1u >= buf_cap) return -1;
            work_buf[used++] = *src++;
        }
        if (quote && *src == quote) src++;
        work_buf[used++] = 0;
        argc++;
    }
    return argc;
}

double jash_chain_jaccard(char** a, int na, char** b, int nb) {
    if (na == 0 && nb == 0) return 1.0;
    int inter = 0;
    for (int i = 0; i < na; i++) {
        for (int j = 0; j < nb; j++) {
            if (a[i] && b[j] && strcmp(a[i], b[j]) == 0) { inter++; break; }
        }
    }
    int uni = na + nb - inter;
    return uni > 0 ? (double)inter / (double)uni : 0.0;
}
