/* SPDX-License-Identifier: LicenseRef-SPL-1.0 */
#define _GNU_SOURCE
#include "journal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>        /* uint64_t — reached transitively on Darwin only */
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>

#define IDLE_THRESHOLD_NS 30000000000ULL

/* Bytes read back from the end of the sidecar to recover the last entry.
 * Entries are ~100 bytes, so this holds the tail many times over while
 * keeping restore O(1) in the size of the journal (JINV-170). */
#define JOURNAL_TAIL_WINDOW 16384

static char g_prev_cmd[256];
static char g_prev_domain[16];
static struct timespec g_last_ts;
static int g_error_count;
static int g_first = 1;
static char g_tag[JOURNAL_TAG_MAX];

static journal_entry_t g_history[JOURNAL_HIST_MAX];
static int g_hcount = 0;

static const char* domain_of(const char* cmd) {
    if (!cmd || !cmd[0]) return "none";
    while (*cmd == ' ') cmd++;
    if (strncmp(cmd, "git", 3) == 0 && (cmd[3] == ' ' || cmd[3] == '\0')) return "git";
    if (strncmp(cmd, "npm", 3) == 0 && (cmd[3] == ' ' || cmd[3] == '\0')) return "npm";
    if (strncmp(cmd, "make", 4) == 0 && (cmd[4] == ' ' || cmd[4] == '\0')) return "build";
    if (strncmp(cmd, "cmake", 5) == 0 && (cmd[5] == ' ' || cmd[5] == '\0')) return "build";
    if (strncmp(cmd, "brew", 4) == 0 && (cmd[4] == ' ' || cmd[4] == '\0')) return "brew";
    if (strncmp(cmd, "ssh", 3) == 0 && (cmd[3] == ' ' || cmd[3] == '\0')) return "ssh";
    if (strncmp(cmd, "docker", 6) == 0 && (cmd[6] == ' ' || cmd[6] == '\0')) return "docker";
    if (strncmp(cmd, "kubectl", 7) == 0 && (cmd[7] == ' ' || cmd[7] == '\0')) return "k8s";
    if (strncmp(cmd, "sudo", 4) == 0 && (cmd[4] == ' ' || cmd[4] == '\0')) return "sudo";
    if (strncmp(cmd, "vi", 2) == 0 && (cmd[2] == ' ' || cmd[2] == '\0')) return "edit";
    if (strncmp(cmd, "vim", 3) == 0 && (cmd[3] == ' ' || cmd[3] == '\0')) return "edit";
    if (strncmp(cmd, "nano", 4) == 0 && (cmd[4] == ' ' || cmd[4] == '\0')) return "edit";
    if (strncmp(cmd, "cd", 2) == 0 && (cmd[2] == ' ' || cmd[2] == '\0')) return "navigate";
    if (strncmp(cmd, "ls", 2) == 0 && (cmd[2] == ' ' || cmd[2] == '\0')) return "explore";
    if (strncmp(cmd, "cat", 3) == 0 && (cmd[3] == ' ' || cmd[3] == '\0')) return "explore";
    if (strncmp(cmd, "grep", 4) == 0 && (cmd[4] == ' ' || cmd[4] == '\0')) return "search";
    if (strncmp(cmd, "explain", 7) == 0 && (cmd[7] == ' ' || cmd[7] == '\0')) return "plan";
    if (strncmp(cmd, "checkpoint", 10) == 0 && (cmd[10] == ' ' || cmd[10] == '\0')) return "plan";
    if (strncmp(cmd, "digest", 6) == 0 && (cmd[6] == ' ' || cmd[6] == '\0')) return "plan";
    if (strncmp(cmd, "timeline", 8) == 0 && (cmd[8] == ' ' || cmd[8] == '\0')) return "review";
    if (strncmp(cmd, "state", 5) == 0 && (cmd[5] == ' ' || cmd[5] == '\0')) return "review";
    if (strncmp(cmd, "undo", 4) == 0 && (cmd[4] == ' ' || cmd[4] == '\0')) return "undo";
    if (strncmp(cmd, "rewind", 6) == 0 && (cmd[6] == ' ' || cmd[6] == '\0')) return "undo";
    if (strncmp(cmd, "recall", 6) == 0 && (cmd[6] == ' ' || cmd[6] == '\0')) return "recall";
    if (strncmp(cmd, "context", 7) == 0 && (cmd[7] == ' ' || cmd[7] == '\0')) return "recall";
    return "cmd";
}

static int is_idle(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    if (g_last_ts.tv_sec == 0 && g_last_ts.tv_nsec == 0) return 0;
    uint64_t elapsed = (uint64_t)(now.tv_sec - g_last_ts.tv_sec) * 1000000000ULL +
                       (uint64_t)(now.tv_nsec - g_last_ts.tv_nsec);
    return elapsed > IDLE_THRESHOLD_NS ? 1 : 0;
}

static int is_retry(const char* cmd) {
    return g_prev_cmd[0] && strcmp(cmd, g_prev_cmd) == 0;
}

static void store_history(journal_tag_kind_t kind, const char* tag, const char* domain, unsigned ec) {
    if (g_hcount >= JOURNAL_HIST_MAX) {
        memmove(g_history, g_history + 1, (JOURNAL_HIST_MAX - 1) * sizeof(journal_entry_t));
        g_hcount = JOURNAL_HIST_MAX - 1;
    }
    journal_entry_t* e = &g_history[g_hcount++];
    memset(e, 0, sizeof(*e));
    e->kind = kind;
    snprintf(e->tag, sizeof(e->tag), "%s", tag);
    if (domain) snprintf(e->domain, sizeof(e->domain), "%s", domain);
    e->error_count = ec;
}

/* Restore previous state from sidecar so cross-process state machine
 * detects domain switches, error bursts, and idle time correctly. */
static void journal_restore_state(void) {
    const char* home = getenv("HOME");
    if (!home || !*home) return;
    char path[600];
    snprintf(path, sizeof(path), "%s/.jash/journal.tags", home);
    FILE* f = fopen(path, "rb");
    if (!f) return;

    /* JINV-170: read ONLY the tail window. This used to fgets() every line of
     * the sidecar and strdup()/free() each one just to keep the last — on a
     * real sidecar (13,021,951 lines / 903 MB on the machine this was found
     * on) that is a full read plus ~13M malloc/free pairs, on EVERY shell
     * start, to recover ONE line. Startup must not scale with how long the
     * shell has been used. Seeking to the end is the same answer in O(1). */
    char win[JOURNAL_TAIL_WINDOW];
    size_t got = 0;
    if (fseek(f, 0, SEEK_END) == 0) {
        long end = ftell(f);
        if (end > 0) {
            long from = end > (long)sizeof win ? end - (long)sizeof win : 0;
            if (fseek(f, from, SEEK_SET) == 0)
                got = fread(win, 1, sizeof win, f);
        }
    }
    fclose(f);
    if (got == 0) return;

    /* Skip trailing newlines, then walk back to the start of that last line. */
    size_t e = got;
    while (e > 0 && (win[e - 1] == '\n' || win[e - 1] == '\r')) e--;
    if (e == 0) return;
    size_t s = e;
    while (s > 0 && win[s - 1] != '\n') s--;

    char tail_buf[1024];
    size_t len = e - s;
    if (len >= sizeof tail_buf) len = sizeof tail_buf - 1;
    memcpy(tail_buf, win + s, len);
    tail_buf[len] = '\0';
    char* last_line = tail_buf;

    /* Parse last entry: <ts> <tag> rc=<rc> cmd=<cmd> */
    char tag[32] = "-";
    int rc = 0;
    char cmd[256] = "";

    char* p = last_line;
    /* Skip timestamp */
    while (*p && *p != ' ') p++;
    if (*p) p++;
    /* Tag */
    {
        size_t i = 0;
        while (*p && *p != ' ' && i < sizeof(tag) - 1) tag[i++] = *p++;
        tag[i] = '\0';
    }
    if (*p) p++;
    /* Parse rc= */
    if (strncmp(p, "rc=", 3) == 0) { rc = atoi(p + 3); while (*p && *p != ' ') p++; }
    if (*p) p++;
    /* Parse cmd= */
    if (strncmp(p, "cmd=", 4) == 0) {
        snprintf(cmd, sizeof(cmd), "%s", p + 4);
        char* nl = strchr(cmd, '\n');
        if (nl) *nl = '\0';
    }

    /* Restore domain from last command */
    const char* domain = domain_of(cmd);
    snprintf(g_prev_cmd, sizeof(g_prev_cmd), "%s", cmd);
    snprintf(g_prev_domain, sizeof(g_prev_domain), "%s", domain);
    g_first = 0;
    g_error_count = (rc != 0) ? 1 : 0;

    /* Replay history entry for last tag */
    if (strcmp(tag, "start") == 0)
        store_history(JOURNAL_TAG_SESSION_START, tag, domain, g_error_count);
    else if (strcmp(tag, "resume") == 0)
        store_history(JOURNAL_TAG_RESUMED, tag, domain, g_error_count);
    else if (strncmp(tag, "sw:", 3) == 0)
        store_history(JOURNAL_TAG_SWITCHED, tag, domain, g_error_count);
    else if (strncmp(tag, "err:", 4) == 0)
        store_history(JOURNAL_TAG_ERROR, tag, domain, g_error_count);
    else if (strcmp(tag, "retry") == 0)
        store_history(JOURNAL_TAG_RETRY, tag, domain, g_error_count);
}

void journal_init(void) {
    g_first = 1;
    g_prev_cmd[0] = '\0';
    g_prev_domain[0] = '\0';
    g_error_count = 0;
    g_last_ts.tv_sec = 0;
    g_last_ts.tv_nsec = 0;
    g_hcount = 0;
    g_tag[0] = '\0';
    /* Sidecar dir: created once here instead of on every appended line. */
    const char* home = getenv("HOME");
    if (home && *home) {
        char dir[512];
        snprintf(dir, sizeof(dir), "%s/.jash", home);
        mkdir(dir, 0700);
    }
    journal_restore_state();
}

void journal_reset(void) { journal_init(); }

/* JINV-170 follow-up: the sidecar had no ceiling (a real one reached 903MB).
 * Same answer as jash_fsdiff_persist's fsjournal: past TRIM_AT, keep only the
 * newest KEEP bytes, line-aligned. The only reader is the tail-window restore,
 * so dropping the old prefix is safe. Best-effort, no lock — a racing append
 * may be lost, exactly like the fsjournal trim. */
#define JOURNAL_TAGS_TRIM_AT (1024 * 1024)
#define JOURNAL_TAGS_KEEP    (256 * 1024)

static void journal_trim_sidecar(const char* path) {
    int in = open(path, O_RDONLY);
    if (in < 0) return;
    struct stat st;
    if (fstat(in, &st) != 0 || st.st_size <= JOURNAL_TAGS_KEEP) {
        close(in);
        return;
    }
    char* tail = (char*)malloc(JOURNAL_TAGS_KEEP);
    if (!tail) { close(in); return; }
    ssize_t got = pread(in, tail, JOURNAL_TAGS_KEEP,
                        st.st_size - JOURNAL_TAGS_KEEP);
    close(in);
    if (got <= 0) { free(tail); return; }
    /* Start at the first full line so the trim never invents a half entry. */
    size_t start = 0;
    while (start < (size_t)got && tail[start] != '\n') start++;
    start = start < (size_t)got ? start + 1 : 0;
    char tmp[640];
    if (snprintf(tmp, sizeof tmp, "%s.tmp.%d", path, (int)getpid()) >=
        (int)sizeof tmp) { free(tail); return; }
    FILE* o = fopen(tmp, "w");
    if (o) {
        fwrite(tail + start, 1, (size_t)got - start, o);
        fclose(o);
        rename(tmp, path);
    }
    free(tail);
}

static void journal_write_sidecar(const char* tag, const char* cmd, int rc) {
    const char* home = getenv("HOME");
    if (!home || !*home) return;
    char path[600];
    snprintf(path, sizeof(path), "%s/.jash/journal.tags", home);
    FILE* f = fopen(path, "a");
    if (!f) return;
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) { fclose(f); return; }
    fprintf(f, "%lld.%09ld %s rc=%d cmd=%s\n",
            (long long)ts.tv_sec, (long)ts.tv_nsec,
            tag && tag[0] ? tag : "-", rc, cmd ? cmd : "");
    long sz = ftell(f);
    fclose(f);
    if (sz > JOURNAL_TAGS_TRIM_AT) journal_trim_sidecar(path);
}

void journal_post_cmd(const char* cmd, int rc) {
    if (!cmd) cmd = "";
    const char* domain = domain_of(cmd);

    if (rc != 0) {
        if (g_error_count < 255) g_error_count++;
    } else {
        g_error_count = 0;
    }

    if (g_first) {
        store_history(JOURNAL_TAG_SESSION_START, "start", domain, g_error_count);
        g_first = 0;
    } else {
        int idle = is_idle();
        if (idle) {
            store_history(JOURNAL_TAG_RESUMED, "resume", domain, g_error_count);
        }
        if (strcmp(domain, g_prev_domain) != 0 && !idle) {
            char swtag[JOURNAL_TAG_MAX];
            snprintf(swtag, sizeof(swtag), "sw:%s", domain);
            store_history(JOURNAL_TAG_SWITCHED, swtag, domain, g_error_count);
        }
        if (rc != 0 && g_error_count >= 2 && g_error_count <= 9) {
            char etag[JOURNAL_TAG_MAX];
            snprintf(etag, sizeof(etag), "err:%d", g_error_count);
            store_history(JOURNAL_TAG_ERROR, etag, domain, g_error_count);
        }
        if (rc != 0 && is_retry(cmd) && g_error_count <= 3) {
            store_history(JOURNAL_TAG_RETRY, "retry", domain, g_error_count);
        }
    }

    /* Persist to sidecar file so journal builtin can query it across processes */
    const char* current_tag = journal_current_tag();
    if (strcmp(current_tag, "-") != 0) {
        journal_write_sidecar(current_tag, cmd, rc);
    }

    if (clock_gettime(CLOCK_MONOTONIC, &g_last_ts) != 0)
        g_last_ts = (struct timespec){0};
    snprintf(g_prev_cmd, sizeof(g_prev_cmd), "%s", cmd);
    snprintf(g_prev_domain, sizeof(g_prev_domain), "%s", domain);
}

const char* journal_current_tag(void) {
    if (g_hcount > 0) {
        journal_entry_t* last = &g_history[g_hcount - 1];
        snprintf(g_tag, sizeof(g_tag), "%s", last->tag);
        return g_tag;
    }
    return "-";
}

const char* journal_auto_tag(const char* cmd, int rc) {
    journal_post_cmd(cmd, rc);
    return journal_current_tag();
}

int journal_tag_history_get(journal_tag_history_t* h, int max) {
    if (!h) return 0;
    int n = g_hcount < max ? g_hcount : max;
    int start = g_hcount - n;
    for (int i = 0; i < n; i++) h->items[i] = g_history[start + i];
    h->count = n;
    h->cap = max;
    return n;
}

void journal_flush(void) {
    /* Journal entries are already written to sidecar from journal_post_cmd.
     * This is a no-op now since we write inline, but kept for future batch I/O. */
}
