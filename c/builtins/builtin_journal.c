/* SPDX-License-Identifier: LicenseRef-SPL-1.0 */
#define _GNU_SOURCE
#include "builtin.h"
#include <core/agent_json.h>
#include <core/string_compat.h>
#include <journal/journal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define JOURNAL_MAX_SHOW 500
#define JOURNAL_LINE_MAX 4096
#define TAG_LEN 32

typedef struct {
    unsigned long long seq;
    unsigned long long ts_sec;
    unsigned long long ts_ns;
    int  rc;
    char tag[TAG_LEN];
    char cmd[256];
} journal_rec_t;

static const char* journal_chain_path(char* buf, size_t cap) {
    const char* home = getenv("HOME");
    if (!home || !*home) return NULL;
    snprintf(buf, cap, "%s/.jash/chain.log", home);
    return buf;
}


static void journal_usage(jash_writer_t w, void* ctx) {
    jash_writes(w, ctx,
        "journal — session timeline with causal auto-tags\n"
        "Usage:\n"
        "  journal [--since <time>] [--tag <tag>] [--failed]\n"
        "         [--last N] [--json] [--what-happened] [pattern]\n"
        "\n"
        "  --since <time>    entries after <time> (\"10m\", \"1h\", \"today\")\n"
        "  --tag <tag>       filter by auto-tag (sw:git, err:2, retry, start, resume, ...)\n"
        "  --failed          only failed commands\n"
        "  --last N          last N matching entries (default: 20)\n"
        "  --json            JSON output\n"
        "  --what-happened   summarise the last pattern break (tag transition)\n"
        "  <pattern>         substring filter on command text\n");
}

static int parse_since_secs(const char* s) {
    if (!s) return -1;
    char* end = NULL;
    long val = strtol(s, &end, 10);
    if (end && *end) {
        if (*end == 'm' || *end == 'M') return (int)(val * 60);
        if (*end == 'h' || *end == 'H') return (int)(val * 3600);
        if (*end == 's' || *end == 'S') return (int)val;
        if (*end == 'd' || *end == 'D') return (int)(val * 86400);
    }
    return (int)val;
}

static void journal_emit_what_happened(jash_writer_t w, void* ctx) {
    journal_tag_history_t hist;
    journal_tag_history_get(&hist, JOURNAL_HIST_MAX);
    if (hist.count == 0) {
        jash_writes(w, ctx, "journal: nothing recorded yet\n");
        return;
    }
    jash_writes(w, ctx, "\033[1;36m═══ Session Timeline — Tags ═══\033[0m\n");
    int start = hist.count > 20 ? hist.count - 20 : 0;
    for (int i = start; i < hist.count; i++) {
        journal_entry_t* e = &hist.items[i];
        const char* kind_str = "?";
        const char* color = "\033[90m";
        switch (e->kind) {
            case JOURNAL_TAG_SESSION_START: kind_str = "START";  color = "\033[1;32m"; break;
            case JOURNAL_TAG_DOMAIN:        kind_str = "DOMAIN"; color = "\033[36m"; break;
            case JOURNAL_TAG_SWITCHED:      kind_str = "SWITCH"; color = "\033[33m"; break;
            case JOURNAL_TAG_ERROR:         kind_str = "ERROR";  color = "\033[1;31m"; break;
            case JOURNAL_TAG_RETRY:         kind_str = "RETRY";  color = "\033[1;33m"; break;
            case JOURNAL_TAG_RESUMED:       kind_str = "RESUME"; color = "\033[35m"; break;
            case JOURNAL_TAG_SUDO:          kind_str = "SUDO";   color = "\033[1;35m"; break;
        }
        char buf[64];
        if (e->error_count > 0) snprintf(buf, sizeof(buf), " [err×%u]", e->error_count);
        else buf[0] = '\0';
        jash_writef(w, ctx, "  %s%-10s\033[0m %s%s  \033[90m%s\033[0m\n",
                    color, kind_str, e->tag, buf, e->domain);
    }
    if (hist.count > 0) {
        journal_entry_t* last = &hist.items[hist.count - 1];
        jash_writef(w, ctx, "\n\033[1m→ Last transition:\033[0m %s \033[90m(domain: %s)\033[0m\n",
                    last->tag, last->domain);
    }
}

static int journal_read_tags(const char* tag_filter, int only_failed, int since_secs,
                              journal_rec_t* ring, int last) {
    const char* home = getenv("HOME");
    if (!home || !*home) return 0;
    char path[600];
    snprintf(path, sizeof(path), "%s/.jash/journal.tags", home);
    FILE* f = fopen(path, "r");
    if (!f) return 0;

    unsigned long long matched = 0;
    char line[JOURNAL_LINE_MAX];
    time_t now = time(NULL);

    while (fgets(line, sizeof(line), f)) {
        /* Format: <ts_sec>.<ts_ns> <tag> rc=<rc> cmd=<cmd> */
        unsigned long long ts_sec = 0, ts_ns = 0;
        char tag[32] = "-";
        int rc = 0;
        char cmd[256] = "";
        sscanf(line, "%llu.%llu %31s rc=%d cmd=%255c", &ts_sec, &ts_ns, tag, &rc, cmd);

        if (only_failed && rc == 0) continue;

        if (tag_filter && strcmp(tag, tag_filter) != 0) continue;

        if (since_secs > 0) {
            time_t cutoff = now - since_secs;
            if ((time_t)ts_sec < cutoff) continue;
        }

        /* Strip trailing newline from cmd */
        size_t cl = strlen(cmd);
        while (cl > 0 && (cmd[cl-1] == '\n' || cmd[cl-1] == '\r')) cmd[--cl] = '\0';

        journal_rec_t* r = &ring[matched % (unsigned long long)last];
        r->seq = matched + 1;
        r->ts_sec = ts_sec;
        r->ts_ns = ts_ns;
        r->rc = rc;
        snprintf(r->tag, sizeof(r->tag), "%s", tag);
        strlcpy(r->cmd, cmd, sizeof(r->cmd));
        r->cmd[sizeof(r->cmd) - 1] = '\0';
        matched++;
    }
    fclose(f);
    return (int)matched;
}

int jash_b_journal(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    (void)s;

    if (!args || !*args) {
        journal_emit_what_happened(w, ctx);
        return 0;
    }

    const char* pattern = NULL;
    const char* tag_filter = NULL;
    int only_failed = 0;
    int json = 0;
    int last = 20;
    int since_secs = -1;
    int what_happened = 0;

    char buf[4096];
    snprintf(buf, sizeof(buf), "%s", args);
    char* save = NULL;
    for (char* t = strtok_r(buf, " \t", &save); t; t = strtok_r(NULL, " \t", &save)) {
        if (strcmp(t, "--failed") == 0) only_failed = 1;
        else if (strcmp(t, "--json") == 0) json = 1;
        else if (strcmp(t, "--what-happened") == 0 || strcmp(t, "--what") == 0) what_happened = 1;
        else if (strcmp(t, "--last") == 0) {
            char* n = strtok_r(NULL, " \t", &save);
            if (n) last = atoi(n);
        } else if (strncmp(t, "--last=", 7) == 0) {
            last = atoi(t + 7);
        } else if (strcmp(t, "--tag") == 0) {
            tag_filter = strtok_r(NULL, " \t", &save);
        } else if (strncmp(t, "--tag=", 6) == 0) {
            tag_filter = t + 6;
        } else if (strcmp(t, "--since") == 0) {
            char* n = strtok_r(NULL, " \t", &save);
            if (n) since_secs = parse_since_secs(n);
        } else if (strncmp(t, "--since=", 8) == 0) {
            since_secs = parse_since_secs(t + 8);
        } else if (strcmp(t, "--help") == 0 || strcmp(t, "-h") == 0) {
            journal_usage(w, ctx);
            return 0;
        } else if (!pattern && t[0] != '-') {
            pattern = t;
        }
    }

    if (what_happened) {
        journal_emit_what_happened(w, ctx);
        return 0;
    }

    if (last < 1) last = 1;
    if (last > JOURNAL_MAX_SHOW) last = JOURNAL_MAX_SHOW;

    journal_rec_t* ring = (journal_rec_t*)calloc((size_t)last, sizeof(journal_rec_t));
    if (!ring) return 1;

    unsigned long long matched = 0;

    /* When filtering by tag, read from sidecar journal.tags which has the
     * real auto-tags. Otherwise read from chain.log. */
    if (tag_filter) {
        matched = (unsigned long long)journal_read_tags(tag_filter, only_failed, since_secs, ring, last);
        if (matched == 0) {
            jash_writes(w, ctx, "journal: no entries with tag '");
            jash_writes(w, ctx, tag_filter);
            jash_writes(w, ctx, "'\n");
            free(ring);
            return 1;
        }
    } else {
        char path[600];
        if (!journal_chain_path(path, sizeof path)) {
            jash_writes(jash_stderr_writer, 0, "journal: no HOME\n");
            free(ring);
            return 1;
        }
        FILE* f = fopen(path, "r");
        if (!f) {
            jash_writes(w, ctx, "journal: no chain yet — set JASH_SIGIL>=1\n");
            free(ring);
            return 1;
        }

        char line[JOURNAL_LINE_MAX];
        time_t now = time(NULL);
        while (fgets(line, sizeof(line), f)) {
            unsigned long long seq = 0, ts_sec = 0, ts_ns = 0;
            char hash[80];
            if (sscanf(line, "%llu %llu.%llu %79s", &seq, &ts_sec, &ts_ns, hash) < 4) continue;
            const char* cp = strstr(line, " cmd=");
            if (!cp) continue;
            const char* cmd = cp + 5;
            if (strncmp(cmd, "SESSION_END", 11) == 0) continue;
            int rc = 0;
            const char* rp = strstr(line, " rc=");
            if (rp) sscanf(rp, " rc=%d", &rc);
            if (only_failed && rc == 0) continue;
            if (pattern && !jash_strcasestr_portable(cmd, pattern)) continue;

            if (since_secs > 0) {
                time_t cutoff = now - since_secs;
                if ((time_t)ts_sec < cutoff) continue;
            }

            journal_rec_t* r = &ring[matched % (unsigned long long)last];
            r->seq = seq; r->ts_sec = ts_sec; r->ts_ns = ts_ns; r->rc = rc;
            snprintf(r->tag, sizeof(r->tag), "-");
            strlcpy(r->cmd, cmd, sizeof(r->cmd)); r->cmd[sizeof(r->cmd) - 1] = '\0';
            for (int i = 0; r->cmd[i]; i++) if (r->cmd[i] == '\n') { r->cmd[i] = '\0'; break; }
            matched++;
        }
        fclose(f);
    }

    if (matched == 0) {
        jash_writes(w, ctx, "journal: no matching entries\n");
        free(ring);
        return 1;
    }

    unsigned long long shown = (matched < (unsigned long long)last) ? matched : (unsigned long long)last;
    for (unsigned long long i = 0; i < shown; i++) {
        unsigned long long idx = (matched - 1 - i) % (unsigned long long)last;
        journal_rec_t* r = &ring[idx];
        if (json) {
            char ecmd[1024];
            jash_json_esc(r->cmd, ecmd, sizeof ecmd);
            char etag[128];
            jash_json_esc(r->tag, etag, sizeof etag);
            jash_writef(w, ctx, "{\"seq\":%llu,\"ts\":%llu,\"rc\":%d,\"tag\":\"%s\",\"cmd\":\"%s\"}\n",
                        r->seq, r->ts_sec, r->rc, etag, ecmd);
            continue;
        }
        char ts[24];
        time_t t = (time_t)r->ts_sec;
        struct tm* tm = localtime(&t);
        if (tm) strftime(ts, sizeof(ts), "%H:%M:%S", tm);
        else snprintf(ts, sizeof(ts), "??:??:??");
        const char* rc_tag = (r->rc == 0) ? "\033[1;32mok\033[0m" : "\033[1;31mFAIL\033[0m";
        const char* tag_color = "\033[36m";
        if (strncmp(r->tag, "err", 3) == 0) tag_color = "\033[1;31m";
        else if (strncmp(r->tag, "retry", 5) == 0) tag_color = "\033[1;33m";
        else if (strncmp(r->tag, "sw:", 3) == 0) tag_color = "\033[33m";
        else if (strncmp(r->tag, "start", 5) == 0) tag_color = "\033[1;32m";
        else if (strncmp(r->tag, "resume", 6) == 0) tag_color = "\033[35m";
        jash_writef(w, ctx, "\033[90m#%llu %s\033[0m %s %s%-8s\033[0m rc=%-3d  %s\n",
                    r->seq, ts, rc_tag, tag_color, r->tag, r->rc, r->cmd);
    }
    free(ring);
    return 0;
}

#include "journal_native.inc"
