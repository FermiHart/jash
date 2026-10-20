// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — core/nudge — see nudge.h. F E R M I ∞ H A R T
#define _GNU_SOURCE
#include "nudge.h"
#include <module/sigil.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#define NUDGE_FAILURE_COOLDOWN 600   /* at most one failure-tip per 10 min */

static int nudge_enabled(void) {
    const char* e = getenv("JASH_NUDGE");
    if (e && e[0] == '0') return 0;            /* explicit opt-out */
    /* Only nudge in recording/agent context — never surprise an interactive
     * human who hasn't opted into the exocortex. */
    return jash_sigil_get() >= SIGIL_LOG;
}

/* In-memory cooldown mirror: without it, every due-check cost a stat() (and
 * every fire an open+write+close) per command. The stamp files remain the
 * cross-process source of truth — the cache only short-circuits checks that
 * THIS process already answered within the cooldown window. */
typedef struct { char tag[64]; long stamp; } nudge_mem_t;
static nudge_mem_t g_mem[8];
static int g_mem_count = 0;
static int g_dirs_made = 0;

static nudge_mem_t* mem_find(const char* tag) {
    for (int i = 0; i < g_mem_count; i++)
        if (strcmp(g_mem[i].tag, tag) == 0) return &g_mem[i];
    return NULL;
}

static void mem_stamp(const char* tag, long stamp) {
    nudge_mem_t* m = mem_find(tag);
    if (!m) {
        if (g_mem_count >= 8) return;  /* few tags in practice; don't grow */
        m = &g_mem[g_mem_count++];
        snprintf(m->tag, sizeof m->tag, "%s", tag);
    }
    m->stamp = stamp;
}

/* Returns 1 if it is time to nudge for `tag` (and stamps the marker), else 0.
 * Exported since ADR-0040 W4: the hint engine (core/hints.c) shares these
 * cooldown stamps so legacy nudges and moment-rules never double-fire. */
int jash_nudge_due(const char* tag, int cooldown) {
    const char* h = getenv("HOME");
    if (!h || !*h) return 0;
    long now = (long)time(NULL);

    /* Fast path: this process already stamped (or observed) the tag inside
     * the cooldown window — no filesystem touch at all. */
    nudge_mem_t* m = mem_find(tag);
    if (m && now - m->stamp < cooldown) return 0;

    if (!g_dirs_made) {
        char dir[512];
        snprintf(dir, sizeof dir, "%s/.jash", h);        mkdir(dir, 0700);
        snprintf(dir, sizeof dir, "%s/.jash/nudges", h); mkdir(dir, 0700);
        g_dirs_made = 1;
    }
    char p[600]; snprintf(p, sizeof p, "%s/.jash/nudges/%s", h, tag);
    struct stat st;
    if (stat(p, &st) == 0 && now - (long)st.st_mtime < cooldown) {
        mem_stamp(tag, (long)st.st_mtime);
        return 0;  /* too soon */
    }
    int fd = open(p, O_CREAT | O_WRONLY | O_TRUNC, 0600);          /* stamp */
    if (fd >= 0) { ssize_t n __attribute__((unused)) = write(fd, "1", 1); close(fd); }
    if (fd >= 0) mem_stamp(tag, now);  /* cache only a stamp that landed */
    return 1;
}

void jash_nudge_failure(jash_writer_t w, void* ctx) {
    (void)w; (void)ctx;
    if (!nudge_enabled()) return;
    if (!jash_nudge_due("failed", NUDGE_FAILURE_COOLDOWN)) return;
    jash_writes(jash_stderr_writer, 0,
        "\033[90m[jash] tip: `recall failed` reviews recent failures · `expect` makes a "
        "wrong assumption stop loudly  (JASH_NUDGE=0 to silence)\033[0m\n");
}
