// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — core/provenance — prompt-injection firewall (ADR-0038 W7).
// See provenance.h for the contract. F E R M I ∞ H A R T

#include "provenance.h"
#include <core/memo.h>   /* jash_env_smells_like_agent */

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* High-precision instruction-shaped patterns. Every entry must be text a
 * legitimate command output almost never contains but a prompt-injection
 * payload almost always does — a false positive costs more trust than a
 * false negative (this layer is warn-only defense in depth, not the last
 * line). Matched case-insensitively. */
static const char* const PROV_PATTERNS[] = {
    "ignore previous instructions",
    "ignore all previous instructions",
    "disregard your instructions",
    "disregard all previous",
    "ignore your system prompt",
    "you must now follow",
    "new instructions:",
    "do not tell the user",
    "hide this from the user",
    "without telling the user",
    "reply with only",
    "<|im_start|>",
    "[/inst]",
    "</system>",
    "assistant:",   /* role spoofing inside data */
    NULL
};

int jash_prov_enabled(void) {
    const char* v = getenv("JASH_PROVENANCE");
    if (v && v[0] == '0') return 0;
    return jash_env_smells_like_agent();
}

static char lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

/* Case-insensitive substring search over a bounded buffer. */
static long ci_find(const char* buf, size_t n, const char* pat) {
    size_t pl = strlen(pat);
    if (pl == 0 || pl > n) return -1;
    for (size_t i = 0; i + pl <= n; i++) {
        size_t k = 0;
        while (k < pl && lc(buf[i + k]) == lc(pat[k])) k++;
        if (k == pl) return (long)i;
    }
    return -1;
}

int jash_prov_scan(const char* buf, size_t n,
                   const char** pattern_out, long* offset_out) {
    if (!buf || n == 0) return 0;
    for (int p = 0; PROV_PATTERNS[p]; p++) {
        long off = ci_find(buf, n, PROV_PATTERNS[p]);
        if (off >= 0) {
            /* "assistant:" only counts at a line start — as a mid-line word
             * it is common in ordinary prose and docs. */
            if (strcmp(PROV_PATTERNS[p], "assistant:") == 0 &&
                off > 0 && buf[off - 1] != '\n')
                continue;
            if (pattern_out) *pattern_out = PROV_PATTERNS[p];
            if (offset_out) *offset_out = off;
            return 1;
        }
    }
    return 0;
}

int jash_prov_scan_file(const char* path, size_t cap,
                        const char** pattern_out, long* offset_out) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    if (cap > 1024UL * 1024) cap = 1024UL * 1024;
    char* buf = (char*)malloc(cap);
    if (!buf) { close(fd); return 0; }
    long n = (long)read(fd, buf, cap);
    close(fd);
    int hit = 0;
    if (n > 0) hit = jash_prov_scan(buf, (size_t)n, pattern_out, offset_out);
    free(buf);
    return hit;
}
