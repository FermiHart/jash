// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — builtins/dashboard — the "measure" layer in one view. Aggregates, from
// the ATTESTED chain + mesh + state, everything about how agents are using jash:
// differential adoption %, activity, top/failing commands, the multi-model fleet,
// the task board, and the safety posture. Human panel + --json for BI pipelines.
// Read-only. F E R M I ∞ H A R T

#define _GNU_SOURCE
#include "builtin.h"
#include <core/agent_json.h>
#include <core/adopt.h>
#include <core/guard.h>
#include <core/hints.h>
#include <module/sigil.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <dirent.h>

static const char* const DIFFS[] = {
    "recall","checkpoint","rewind","expect","explain","mesh",
    "undo","timeline","trash","sigil","kv","context","verify","doctor","dashboard",
    "speculate","revert","cost","contract","state","digest","json","agent-guard", NULL
};
static int diff_idx(const char* w) {
    for (int i = 0; DIFFS[i]; i++) if (!strcmp(w, DIFFS[i])) return i;
    return -1;
}

static int is_cmdword_char(char c) {
    return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_';
}

/* The Claude Code shell tool wraps EVERY command as
 *   setopt … && eval '<real cmd>' && pwd …
 * so word 0 is always "setopt" and the real verb hides at a command position
 * INSIDE the line. Scan command positions (start, after ; & | newline ( { `,
 * and the string given to eval/sh/bash) for the first differentiator. */
static int scan_differentiator(const char* cmd) {
    const char* p = cmd; int at_cmd = 1;
    while (*p) {
        if (*p == ' ' || *p == '\t') { p++; continue; }
        if (at_cmd) {
            while (*p == '\'' || *p == '"') p++;
            while (*p == ' ' || *p == '\t') p++;
            if (!*p) break;
            char w[48]; unsigned wi = 0;
            while (wi < sizeof(w)-1 && (is_cmdword_char(p[wi]) || p[wi] == '-')) {
                w[wi] = p[wi]; wi++;
            }
            w[wi] = '\0';
            int idx = diff_idx(w);
            if (idx >= 0) return idx;
            at_cmd = (!strcmp(w,"eval")||!strcmp(w,"sh")||!strcmp(w,"bash")||!strcmp(w,"command")) ? 1 : 0;
            p += wi ? wi : 1;
            continue;
        }
        if (*p==';'||*p=='\n'||*p=='('||*p=='{'||*p=='`') { at_cmd = 1; p++; continue; }
        if (*p=='&'||*p=='|') { p++; if (*p=='&'||*p=='|') p++; at_cmd = 1; continue; }
        p++;
    }
    return -1;
}

/* The REAL command word (for the "most run" stat) — the first command-position
 * word that isn't a wrapper artifact (setopt/eval/true/pwd/…). Without this the
 * top-commands list is just "setopt" N times. */
static void real_cmd_word(const char* cmd, char* out, unsigned outsz) {
    const char* p = cmd; int at_cmd = 1; out[0] = '\0';
    while (*p) {
        if (*p == ' ' || *p == '\t') { p++; continue; }
        if (at_cmd) {
            while (*p == '\'' || *p == '"') p++;
            while (*p == ' ' || *p == '\t') p++;
            if (!*p) break;
            char w[48]; unsigned wi = 0;
            while (wi < sizeof(w)-1 && (is_cmdword_char(p[wi]) || p[wi] == '-')) {
                w[wi] = p[wi]; wi++;
            }
            w[wi] = '\0';
            if (wi && strcmp(w,"setopt") && strcmp(w,"eval") && strcmp(w,"true")
                   && strcmp(w,"pwd") && strcmp(w,"command") && strcmp(w,"sh") && strcmp(w,"bash")) {
                unsigned k = 0; for (; w[k] && k < outsz-1; k++) out[k] = w[k]; out[k] = '\0'; return;
            }
            at_cmd = (!strcmp(w,"eval")||!strcmp(w,"sh")||!strcmp(w,"bash")||!strcmp(w,"command")) ? 1 : 0;
            p += wi ? wi : 1;
            continue;
        }
        if (*p==';'||*p=='\n'||*p=='('||*p=='{'||*p=='`') { at_cmd = 1; p++; continue; }
        if (*p=='&'||*p=='|') { p++; if (*p=='&'||*p=='|') p++; at_cmd = 1; continue; }
        p++;
    }
    if (!out[0]) { const char* q = cmd; while (*q==' ') q++;
        unsigned k = 0; while (is_cmdword_char(q[k]) && k < outsz-1) { out[k]=q[k]; k++; } out[k]='\0'; }
}

#define DASH_TOP 512
typedef struct { char word[48]; unsigned runs, fails; } cmdstat_t;

static int home_path(const char* sub, char* b, size_t cap) {
    const char* h = getenv("HOME"); if (!h || !*h) return 0;
    snprintf(b, cap, "%s/%s", h, sub); return 1;
}

#include "dashboard_semantic.inc"

int jash_b_dashboard(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    int structured = semantic_dispatch(s, w, ctx, "dashboard", "jash.dashboard.v1", args, dashboard_semantic);
    if (structured >= 0) return structured;
    (void)s;
    int json = (args && strstr(args, "--json"));

    /* ── chain: activity + adoption + top commands ── */
    unsigned total = 0, fails = 0, diff_uses = 0;
    unsigned per[32] = {0};
    unsigned long long first_ts = 0, last_ts = 0;
    cmdstat_t* tab = (cmdstat_t*)calloc(DASH_TOP, sizeof(cmdstat_t));
    unsigned ndist = 0;
    char path[600];
    const char* repo = jash_repo_key();
    int have_chain = repo && repo[0] != '-' && !getenv("JASH_RECALL_GLOBAL")
        ? (home_path(".jash", path, sizeof path) &&
           (size_t)snprintf(path, sizeof path, "%s/.jash/chain-%s.log",
                            getenv("HOME"), repo) < sizeof path)
        : home_path(".jash/chain.log", path, sizeof path);
    if (tab && have_chain) {
        FILE* f = fopen(path, "r");
        if (f) {
            char line[1024];
            while (fgets(line, sizeof line, f)) {
                unsigned long long seq, ts, ns; char hash[80];
                if (sscanf(line, "%llu %llu.%llu %79s", &seq, &ts, &ns, hash) < 4) continue;
                const char* cp = strstr(line, " cmd="); if (!cp) continue;
                const char* cmd = cp + 5;
                if (strncmp(cmd, "SESSION_END", 11) == 0) continue;
                int rc = 0; const char* rp = strstr(line, " rc=");
                if (rp) sscanf(rp, " rc=%d", &rc);
                char word[48]; real_cmd_word(cmd, word, sizeof word);
                unsigned wi = (unsigned)strlen(word); if (!wi) continue;
                if (!first_ts) { first_ts = ts; }
                last_ts = ts;
                total++; if (rc) fails++;
                int di = scan_differentiator(cmd); if (di >= 0) { diff_uses++; if (di < 32) per[di]++; }
                unsigned i = 0; for (; i < ndist; i++) if (!strcmp(tab[i].word, word)) break;
                if (i == ndist && ndist < DASH_TOP) { strncpy(tab[i].word, word, sizeof(tab[i].word)-1); ndist++; }
                if (i < DASH_TOP) { tab[i].runs++; if (rc) tab[i].fails++; }
            }
            fclose(f);
        }
    }
    int adopt = total ? (int)((diff_uses*100 + total/2)/total) : 0;
    int failpct = total ? (int)((fails*100 + total/2)/total) : 0;

    /* ── mesh: fleet + board ── */
    int fleet = 0, b_total=0, b_done=0, b_claimed=0, b_ready=0, b_failed=0;
    char wkdir[680];
    if (home_path(".jash/mesh/workers", wkdir, sizeof wkdir)) {
        DIR* d = opendir(wkdir);
        if (d) { struct dirent* e; while ((e=readdir(d))) if (e->d_name[0] != '.') fleet++; closedir(d); }
    }
    char tkdir[680];
    if (home_path(".jash/mesh/tasks", tkdir, sizeof tkdir)) {
        DIR* d = opendir(tkdir);
        if (d) { struct dirent* e;
            while ((e=readdir(d))) { if (e->d_name[0]=='.') continue;
                char fp[1400]; snprintf(fp,sizeof fp,"%s/%s",tkdir,e->d_name);
                FILE* tf=fopen(fp,"r"); if(!tf) continue; char ln[1024];
                if (fgets(ln,sizeof ln,tf)) { b_total++;
                    if (strstr(ln,"\tdone\t")) b_done++; else if (strstr(ln,"\tclaimed\t")) b_claimed++;
                    else if (strstr(ln,"\tready\t")) b_ready++; else if (strstr(ln,"\tfailed\t")) b_failed++; }
                fclose(tf); }
            closedir(d); }
    }

    /* ── agent anti-bypass metrics ── */
    unsigned m_total = 0, m_native = 0, m_bypass = 0, m_blocks = 0;
    jash_guard_agent_status(NULL, NULL, &m_total, &m_native, &m_bypass, &m_blocks);
    int bypass_pct = m_total ? (int)((m_bypass * 100 + m_total/2) / m_total) : 0;

    /* ── safety posture ── */
    int sigil = jash_sigil_get();
    int protect = jash_trash_protected();
    int ncp = 0;
    if (home_path(".jash/checkpoints.tsv", path, sizeof path)) {
        FILE* f = fopen(path, "r"); if (f) { char ln[1024]; while (fgets(ln,sizeof ln,f)) ncp++; fclose(f); }
    }
    long uptime = (first_ts && last_ts) ? (long)(last_ts - first_ts) : 0;

    /* find top-N helpers operate on the tab */
    if (json) {
        jash_writef(w, ctx,
            "{\"recording\":{\"level\":%d,\"trash_protected\":%s},"
            "\"commands\":%u,\"failed\":%u,\"fail_pct\":%d,\"uptime_s\":%ld,"
            "\"adoption\":{\"pct\":%d,\"differentiator_uses\":%u,\"features\":{",
            sigil, protect?"true":"false", total, fails, failpct, uptime, adopt, diff_uses);
        int c=0; for (int i=0; DIFFS[i] && i<32; i++) if (per[i]) { jash_writef(w,ctx,"%s\"%s\":%u",c?",":"",DIFFS[i],per[i]); c++; }
        jash_writef(w, ctx, "}},\"bypass\":{\"total\":%u,\"bypass_count\":%u,\"block_count\":%u,\"pct\":%d},"
                            "\"governor\":{\"level\":%d,\"hints_used\":%d,\"hint_budget\":%d},"
                            "\"fleet\":%d,\"board\":{\"total\":%d,\"done\":%d,\"claimed\":%d,\"ready\":%d,\"failed\":%d},"
                            "\"checkpoints\":%d}\n",
            m_total, m_bypass, m_blocks, bypass_pct,
            jash_adopt_level(), jash_hint_budget_used(), jash_hint_budget_cap(),
            fleet, b_total, b_done, b_claimed, b_ready, b_failed, ncp);
        free(tab); return 0;
    }

    /* ── human panel ── */
    jash_writes(w, ctx, "\033[1;36m═══ JASH DASHBOARD — agent usage ═══\033[0m\r\n");
    if (sigil < SIGIL_LOG)
        jash_writes(w, ctx, "  \033[1;31mrecording OFF\033[0m — set JASH_SIGIL>=1 (the metrics need the attested chain)\r\n");
    {
        long m = uptime/60, h = m/60; m %= 60;
        jash_writef(w, ctx, "  recording \033[1m%s\033[0m (sigil %d) · trash \033[1m%s\033[0m · %u commands",
            sigil>=SIGIL_LOG?"ON":"OFF", sigil, protect?"ON":"OFF", total);
        if (uptime) jash_writef(w, ctx, " over %ldh%02ldm", h, m);
        jash_writes(w, ctx, "\r\n");
    }
    /* adoption bar */
    {
        char bar[11]; int fill = adopt/10; for (int i=0;i<10;i++) bar[i] = i<fill?'#':'.'; bar[10]='\0';
        const char* col = adopt>=40?"32":(adopt>=15?"33":"31");
        jash_writef(w, ctx, "\r\n  \033[1mADOPTION\033[0m  \033[1;%sm%s %d%%\033[0m  (%u of %u commands used a differential)\r\n",
            col, bar, adopt, diff_uses, total);
        jash_writes(w, ctx, "    ");
        int any=0; for (int i=0; DIFFS[i] && i<32; i++) if (per[i]) { jash_writef(w,ctx,"%s %u  ",DIFFS[i],per[i]); any=1; }
        jash_writes(w, ctx, any ? "\r\n" : "\033[31m(none — agents are using jash as plain bash)\033[0m\r\n");
    }
    /* activity: top run + top failed */
    jash_writef(w, ctx, "\r\n  \033[1mACTIVITY\033[0m  %u run · %u failed (%d%%)\r\n", total, fails, failpct);
    jash_writes(w, ctx, "    most run:    ");
    for (int rank=0; rank<5; rank++) { int best=-1; for (unsigned i=0;i<ndist;i++) if (tab[i].runs && (best<0||tab[i].runs>tab[best].runs)) best=(int)i;
        if (best<0) { break; }
        jash_writef(w,ctx,"%s %u  ",tab[best].word,tab[best].runs); tab[best].runs=0; }
    jash_writes(w, ctx, "\r\n");
    { int anyf=0; for (unsigned i=0;i<ndist;i++) if (tab[i].fails) { anyf=1; break; }
      if (anyf) { jash_writes(w, ctx, "    most failed: ");
        for (int rank=0; rank<5; rank++) { int best=-1; for (unsigned i=0;i<ndist;i++) if (tab[i].fails && (best<0||tab[i].fails>tab[best].fails)) best=(int)i;
            if (best<0) { break; }
            jash_writef(w,ctx,"\033[31m%s %u\033[0m  ",tab[best].word,tab[best].fails); tab[best].fails=0; }
        jash_writes(w, ctx, "\r\n"); } }
    /* fleet + board */
    if (fleet || b_total) {
        jash_writef(w, ctx, "\r\n  \033[1mMESH\033[0m  %d worker(s) · board: %d task(s)", fleet, b_total);
        if (b_total) jash_writef(w, ctx, " (\033[32m%d done\033[0m · %d claimed · %d ready · \033[31m%d failed\033[0m)", b_done,b_claimed,b_ready,b_failed);
        jash_writes(w, ctx, "  \033[90m— mesh workers / mesh board\033[0m\r\n");
    }
    /* bypass */
    {
        const char* col = bypass_pct >= 20 ? "31" : (bypass_pct >= 5 ? "33" : "32");
        jash_writef(w, ctx, "\r\n  \033[1mBYPASS\033[0m  \033[1;%sm%d%%\033[0m  (%u of %u commands escaped to bash/sh/python; %u blocked)\r\n",
            col, bypass_pct, m_bypass, m_total, m_blocks);
    }
    /* governor + hints (ADR-0040) */
    {
        int lvl = jash_adopt_level();
        int hu = jash_hint_budget_used(), hc = jash_hint_budget_cap();
        static const char* LVL[] = {"0 off","1 nudge","2 rewrite","3 strict"};
        jash_writef(w, ctx, "\r\n  \033[1mGOVERNOR\033[0m  level \033[1m%s\033[0m · hints %d/%d this session  \033[90m— adopt / JASH_ADOPT\033[0m\r\n",
            (lvl>=0&&lvl<=3)?LVL[lvl]:"?", hu, hc);
    }
    /* safety */
    jash_writef(w, ctx, "  \033[1mSAFETY\033[0m  %d checkpoint(s) · rm %s\r\n",
        ncp, protect?"recoverable (auto-checkpoint armed)":"\033[31mpermanent (trash off)\033[0m");
    free(tab);
    return 0;
}

SEMANTIC_ARGV_WRAPPER(dashboard, "dashboard", "jash.dashboard.v1", dashboard_semantic)
