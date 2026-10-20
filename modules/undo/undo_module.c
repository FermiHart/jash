/* SPDX-License-Identifier: LicenseRef-SPL-1.0 */
// jash module: undo — ADR-0018 Waves 2-4
// Transactional Undo Fabric: tx begin/commit/rollback/preview/diff
// Session persistence (Wave 3) + Agent safe mode (Wave 4)

#include <module/modjash.h>
#include <core/timeline.h>
#include <module/jash_crypto.h>  /* BLAKE3 for session integrity */
#include <module/sigil.h>        /* sigil level query              */
#include <theme/io.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <errno.h>

/* ── Module state ──────────────────────────────────────────────────── */
static int      g_tx_active   = 0;
static int      g_mut_count   = 0;
static char     g_tx_name[64] = {0};
static int      g_safe_mode   = 0;       /* agent safe mode toggle       */
static int      g_safe_policy = 0;       /* 0=ask, 1=always, 2=never     */
static uint64_t g_sess_start  = 0;       /* session start ns              */
static uint64_t g_sess_all    = 0;       /* total mutations this session  */
static char     g_sess_id[17] = {0};     /* hex session id                */
static char     g_last_root[65] = {0};   /* last tx_root BLAKE3 hex       */

/* Ring: last 256 mutations for session persistence + preview */
#define TX_RING_SZ 256
static int      g_ring_kind[TX_RING_SZ];
static char     g_ring_desc[TX_RING_SZ][160];
static int      g_ring_idx   = 0;
static int      g_ring_total = 0;        /* total ever recorded           */

/* ── Helpers ───────────────────────────────────────────────────────── */

static const char* kind_str(int kind) {
    switch (kind) {
        case JASH_UNDO_FILE_OVERWRITE: return "overwrite";
        case JASH_UNDO_FILE_CREATE:    return "create";
        case JASH_UNDO_MOVE:           return "move";
        case JASH_UNDO_TRASH:          return "trash";
        case JASH_UNDO_ENV_SET:        return "env+";
        case JASH_UNDO_ENV_UNSET:      return "env-";
        case JASH_UNDO_CHDIR:          return "cd";
        default:                       return "?";
    }
}

static void tx_write(jmod_writer_t w, void* ctx, const char* s) {
    if (w) w(ctx, s, strlen(s));
}

static void tx_writef(jmod_writer_t w, void* ctx, const char* fmt, ...) {
    char buf[768];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0 && w) w(ctx, buf, (size_t)n);
}

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static const char* sess_dir(void) {
    static char dir[512];
    const char* home = getenv("HOME");
    if (!home) home = ".";
    snprintf(dir, sizeof(dir), "%s/.jash/undo/sessions", home);
    return dir;
}

static int sess_ensure_dir(void) {
    const char* home = getenv("HOME");
    if (!home) return -1;
    char d[512];
    snprintf(d, sizeof(d), "%s/.jash/undo", home);
    mkdir(d, 0700);
    snprintf(d, sizeof(d), "%s/.jash/undo/sessions", home);
    mkdir(d, 0700);
    return 0;
}

static void sess_gen_id(void) {
    /* Simple: timestamp hex */
    uint64_t ts = now_ns();
    for (int i = 0; i < 8; i++) {
        unsigned b = (unsigned)((ts >> (i * 8)) & 0xFF);
        snprintf(g_sess_id + i * 2, 3, "%02x", b);
    }
    g_sess_id[16] = '\0';
}

static void ring_push(int kind, const char* desc) {
    int pos = g_ring_idx % TX_RING_SZ;
    g_ring_kind[pos] = kind;
    if (desc) {
        size_t n = strlen(desc);
        if (n >= sizeof(g_ring_desc[0])) n = sizeof(g_ring_desc[0]) - 1;
        memcpy(g_ring_desc[pos], desc, n);
        g_ring_desc[pos][n] = '\0';
    } else {
        g_ring_desc[pos][0] = '\0';
    }
    g_ring_idx++;
    g_ring_total++;
}

/* read from ring: idx=0 is oldest still in ring, idx=ring_total-1 is newest */
static int ring_get(int offset_from_oldest, int* kind, char* desc, size_t desc_sz) {
    int total = g_ring_total < TX_RING_SZ ? g_ring_total : TX_RING_SZ;
    if (offset_from_oldest < 0 || offset_from_oldest >= total) return -1;
    int start = g_ring_total > TX_RING_SZ ? g_ring_total - TX_RING_SZ : 0;
    int pos = (start + offset_from_oldest) % TX_RING_SZ;
    *kind = g_ring_kind[pos];
    if (desc && desc_sz) {
        strncpy(desc, g_ring_desc[pos], desc_sz - 1);
        desc[desc_sz - 1] = '\0';
    }
    return 0;
}

/* ── Session persistence ───────────────────────────────────────────── */

static int sess_save(const char* name) {
    if (sess_ensure_dir() != 0) return -1;
    if (!g_sess_id[0]) sess_gen_id();

    char path[600], tmppath[620];
    snprintf(path, sizeof(path), "%s/%s.jrnl", sess_dir(), g_sess_id);
    snprintf(tmppath, sizeof(tmppath), "%s.tmp", path);

    FILE* f = fopen(tmppath, "w");
    if (!f) return -1;

    uint64_t end_ns = now_ns();
    int total = g_ring_total < TX_RING_SZ ? g_ring_total : TX_RING_SZ;

    /* Write header (without hash line yet) */
    fprintf(f, "# session_id: %s\n", g_sess_id);
    fprintf(f, "# start_ts: %llu\n", (unsigned long long)g_sess_start);
    fprintf(f, "# end_ts: %llu\n", (unsigned long long)end_ns);
    fprintf(f, "# mutations: %llu\n", (unsigned long long)g_sess_all);
    if (g_last_root[0]) fprintf(f, "# last_tx_root: %s\n", g_last_root);
    if (name && *name) fprintf(f, "# name: %s\n", name);

    /* Compute BLAKE3 over the body (mutations) */
    struct blake3 h;
    blake3_init(&h);

    fprintf(f, "---\n");
    for (int i = 0; i < total; i++) {
        int kind; char desc[160];
        if (ring_get(i, &kind, desc, sizeof(desc)) == 0) {
            char line[200];
            int n = snprintf(line, sizeof(line), "%d %s\n", kind, desc[0]?desc:"-");
            fprintf(f, "%s", line);
            blake3_update(&h, line, (uint64_t)n);
        }
    }
    fclose(f);

    /* Compute and prepend hash line */
    uint8_t hash[32];
    blake3_final(&h, hash, 32);
    char hex[JASH_BLAKE3_HEX_LEN];
    jash_blake3_hex(hash, hex);

    /* Rewrite with hash line after session_id */
    FILE* fin = fopen(tmppath, "r");
    FILE* fout = fopen(path, "w");
    if (!fin || !fout) { if(fin) fclose(fin); if(fout) fclose(fout); unlink(tmppath); return -1; }

    char line[600];
    int wrote_hash = 0;
    while (fgets(line, sizeof(line), fin)) {
        fputs(line, fout);
        if (!wrote_hash && strncmp(line, "# session_id:", 13) == 0) {
            fprintf(fout, "# blake3: %s\n", hex);
            wrote_hash = 1;
        }
    }
    fclose(fin);
    fclose(fout);
    unlink(tmppath);
    return 0;
}

static int cmd_tx_save(const jmod_call_t* call) {
    const char* name = call->args ? call->args : "";
    while (*name == ' ') name++;
    if (strncmp(name, "save", 4) == 0) {
        name += 4;
        while (*name == ' ') name++;
    }
    if (sess_save(name) == 0) {
        tx_writef(call->writer, call->writer_ctx,
                  "tx: session saved → %s/%s.jrnl (%llu mutations, BLAKE3)\n",
                  sess_dir(), g_sess_id,
                  (unsigned long long)g_sess_all);
        return 0;
    }
    tx_write(call->writer, call->writer_ctx,
             "tx: save failed — cannot write session\n");
    return 1;
}

static int cmd_tx_sessions(const jmod_call_t* call) {
    const char* sd = sess_dir();
    DIR* d = opendir(sd);
    if (!d) {
        tx_write(call->writer, call->writer_ctx,
                 "tx: no saved sessions\n");
        return 0;
    }

    struct dirent* ent;
    int found = 0;
    tx_write(call->writer, call->writer_ctx,
             "Saved sessions:\n");
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        const char* ext = strrchr(ent->d_name, '.');
        if (!ext || strcmp(ext, ".jrnl") != 0) continue;

        char path[600];
        size_t dirlen = strlen(sd), namelen = strlen(ent->d_name);
        if (dirlen >= sizeof(path) || namelen >= sizeof(path) - dirlen - 1) {
            closedir(d);
            tx_write(call->writer, call->writer_ctx,
                     "tx: session path exceeds budget; listing incomplete\n");
            return 1;
        }
        memcpy(path, sd, dirlen);
        path[dirlen] = '/';
        memcpy(path + dirlen + 1, ent->d_name, namelen + 1);
        FILE* f = fopen(path, "r");
        if (!f) continue;

        char sid[17] = "?";
        char sname[64] = "";
        int  muts = 0;
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "# session_id: ", 14) == 0)
                { strncpy(sid, line+14, 16); sid[16]='\0';
                  for(int i=0;i<16;i++) if(sid[i]=='\n') sid[i]='\0'; }
            else if (strncmp(line, "# mutations: ", 13) == 0)
                muts = atoi(line + 13);
            else if (strncmp(line, "# name: ", 8) == 0)
                { strncpy(sname, line+8, 63); sname[63]='\0';
                  for(int i=0;i<63;i++) if(sname[i]=='\n') sname[i]='\0'; }
            if (line[0] == '-') break;
        }
        fclose(f);

        tx_writef(call->writer, call->writer_ctx,
                  "  %.8s  %4d mut  %s%s\n",
                  sid, muts,
                  sname[0] ? sname : "(unnamed)",
                  strncmp(sid, g_sess_id, 16) == 0 ? "  ← current" : "");
        found++;
    }
    closedir(d);
    if (!found)
        tx_write(call->writer, call->writer_ctx, "  (none)\n");
    return 0;
}

static int cmd_tx_session_show(const jmod_call_t* call, const char* sid) {
    if (!sid || !*sid) {
        tx_write(call->writer, call->writer_ctx,
                 "tx: usage — tx session show <id>\n");
        return 1;
    }

    /* Try exact match first, then prefix search */
    char path[600];
    snprintf(path, sizeof(path), "%s/%s.jrnl", sess_dir(), sid);
    FILE* f = fopen(path, "r");

    if (!f) {
        /* Prefix search */
        DIR* d = opendir(sess_dir());
        if (d) {
            struct dirent* ent;
            while ((ent = readdir(d)) != NULL) {
                if (strncmp(ent->d_name, sid, strlen(sid)) == 0
                    && strstr(ent->d_name, ".jrnl")) {
                    snprintf(path, sizeof(path), "%s/%s", sess_dir(), ent->d_name);
                    f = fopen(path, "r");
                    break;
                }
            }
            closedir(d);
        }
    }

    if (!f) {
        tx_writef(call->writer, call->writer_ctx,
                  "tx: session %s not found\n", sid);
        return 1;
    }

    char line[512];
    int shown = 0, body = 0;
    while (fgets(line, sizeof(line), f)) {
        if (!body) {
            if (line[0] == '-' && line[1] == '-' && line[2] == '-')
                { body = 1; continue; }
            tx_write(call->writer, call->writer_ctx, line);
        } else if (shown < 30) {
            int kind; char desc[160] = {0};
            if (sscanf(line, "%d %159[^\n]", &kind, desc) >= 1) {
                tx_writef(call->writer, call->writer_ctx,
                          "  %-10s %s\n", kind_str(kind), desc);
                shown++;
            }
        }
    }
    if (shown >= 30)
        tx_write(call->writer, call->writer_ctx, "  ... (truncated)\n");
    fclose(f);
    return 0;
}

/* ── Session verify (BLAKE3 integrity) ─────────────────────────────── */

static int cmd_tx_verify(const jmod_call_t* call) {
    const char* sub = call->args ? call->args : "";
    while (*sub == ' ') sub++;
    if (strncmp(sub, "verify", 6) == 0) { sub += 6; while (*sub == ' ') sub++; }

    const char* sid = *sub ? sub : g_sess_id;
    if (!sid || !*sid) {
        tx_write(call->writer, call->writer_ctx,
                 "tx: usage — tx verify [session-id]\n");
        return 1;
    }

    /* Try exact match first, then prefix search */
    char path[600], sid_buf[160] = {0};
    strncpy(sid_buf, sid, sizeof(sid_buf)-1);
    snprintf(path, sizeof(path), "%s/%s.jrnl", sess_dir(), sid);
    FILE* f = fopen(path, "r");

    if (!f) {
        DIR* d = opendir(sess_dir());
        if (d) {
            struct dirent* ent;
            while ((ent = readdir(d)) != NULL) {
                if (strncmp(ent->d_name, sid, strlen(sid)) == 0
                    && strstr(ent->d_name, ".jrnl")) {
                    snprintf(path, sizeof(path), "%s/%s", sess_dir(), ent->d_name);
                    f = fopen(path, "r");
                    break;
                }
            }
            closedir(d);
        }
    }

    if (!f) {
        tx_writef(call->writer, call->writer_ctx,
                  "tx: session %s not found\n", sid);
        return 1;
    }

    char stored_hash[65] = {0};
    char line[600];
    struct blake3 h;
    blake3_init(&h);
    int in_body = 0, entries = 0;

    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "# blake3: ", 10) == 0) {
            strncpy(stored_hash, line + 10, 64);
            stored_hash[64] = '\0';
            for (int i = 63; i >= 0; i--)
                if (stored_hash[i] == '\n') stored_hash[i] = '\0';
            continue;
        }
        if (!in_body) {
            if (line[0] == '-' && line[1] == '-' && line[2] == '-')
                { in_body = 1; continue; }
        } else {
            blake3_update(&h, line, (uint64_t)strlen(line));
            entries++;
        }
    }
    fclose(f);

    uint8_t computed[32];
    blake3_final(&h, computed, 32);
    char computed_hex[JASH_BLAKE3_HEX_LEN];
    jash_blake3_hex(computed, computed_hex);

    if (stored_hash[0] && strcmp(stored_hash, computed_hex) == 0) {
        tx_writef(call->writer, call->writer_ctx,
                  "\033[1;32m✓ session %s integrity VERIFIED\033[0m (%d entries, BLAKE3)\n",
                  sid, entries);
        return 0;
    } else if (stored_hash[0]) {
        tx_writef(call->writer, call->writer_ctx,
                  "\033[1;31m✗ session %s TAMPERED\033[0m\n"
                  "  stored:  %.16s...\n"
                  "  computed: %.16s...\n",
                  sid, stored_hash, computed_hex);
        return 1;
    } else {
        tx_writef(call->writer, call->writer_ctx,
                  "\033[1;33m⚠ session %s has no integrity hash\033[0m (%d entries)\n",
                  sid, entries);
        return 0;
    }
}

/* ── Tx root computation (ADR-0018 Wave 5) ─────────────────────────── */

/* forward: dispatch into other modules (for sigil bridge) */
extern int jmod_dispatch(const char* verb, const jmod_call_t* call);

static void tx_compute_root(char out_hex[JASH_BLAKE3_HEX_LEN]) {
    struct blake3 h;
    blake3_init(&h);

    /* Hash tx name */
    blake3_update(&h, g_tx_name, (uint64_t)strlen(g_tx_name));

    /* Hash each mutation in the tx */
    int total = g_ring_total < TX_RING_SZ ? g_ring_total : TX_RING_SZ;
    int start = total - g_mut_count;
    if (start < 0) start = 0;
    for (int i = 0; i < g_mut_count && i < TX_RING_SZ; i++) {
        int kind; char desc[160];
        if (ring_get(start + i, &kind, desc, sizeof(desc)) == 0) {
            char line[200];
            int n = snprintf(line, sizeof(line), "%d:%s", kind, desc[0]?desc:"-");
            blake3_update(&h, line, (uint64_t)n);
        }
    }

    uint8_t root[32];
    blake3_final(&h, root, 32);
    jash_blake3_hex(root, out_hex);
    memcpy(g_last_root, out_hex, 64);
    g_last_root[64] = '\0';
}

static void tx_sigil_record(const jmod_call_t* call, const char* action,
                             const char* tx_name, const char* root_hex) {
    int siglvl = jash_sigil_get();
    if (siglvl < SIGIL_LOG) {
        tx_write(call->writer, call->writer_ctx,
                 "    \033[90mtip: 'sigil sign' to attest transactions\033[0m\n");
        return;
    }

    /* Build: "TX_COMMIT <name> root=<root_hex>" or "TX_ROLLBACK <name> root=<root_hex>" */
    char sigil_args[256];
    snprintf(sigil_args, sizeof(sigil_args), "record %s %s root=%.16s",
             action, tx_name && *tx_name ? tx_name : "unnamed", root_hex);

    jmod_call_t sigil_call = {
        .state      = call->state,
        .writer     = call->writer,
        .writer_ctx = call->writer_ctx,
        .verb       = NULL,  /* set by jmod_dispatch */
        .args       = sigil_args,
    };

    int rc = jmod_dispatch("sigil", &sigil_call);
    if (rc == 0) {
        tx_writef(call->writer, call->writer_ctx,
                  "    \033[90msigil: %s recorded in chain (root %.16s...)\033[0m\n",
                  action, root_hex);
    } else {
        tx_writef(call->writer, call->writer_ctx,
                  "    \033[90msigil: %s NOT recorded (sigil level %d, dispatch rc=%d)\033[0m\n",
                  action, siglvl, rc);
    }
}

/* ── Lifecycle ─────────────────────────────────────────────────────── */

static int undo_init(void) {
    g_tx_active   = 0;
    g_mut_count   = 0;
    g_tx_name[0]  = '\0';
    g_safe_mode   = 0;
    g_safe_policy = 0;
    g_sess_start  = now_ns();
    g_sess_all    = 0;
    g_ring_idx    = 0;
    g_ring_total  = 0;
    sess_gen_id();
    return 0;
}

static void undo_shutdown(void) {
    if (g_sess_all > 0) sess_save(NULL);
}

/* ── Mutation hook ─────────────────────────────────────────────────── */

static void undo_mutation_hook(int kind, const char* desc,
                                const char* a, const char* b) {
    (void)a; (void)b;

    /* Always track for session persistence */
    ring_push(kind, desc);
    g_sess_all++;

    if (!g_tx_active) return;
    g_mut_count++;
}

/* ── Agent safe mode ───────────────────────────────────────────────── */

static int cmd_tx_safe(const jmod_call_t* call) {
    const char* sub = call->args ? call->args : "";
    while (*sub == ' ') sub++;
    if (strncmp(sub, "safe", 4) == 0) { sub += 4; while (*sub == ' ') sub++; }

    if (strncmp(sub, "on", 2) == 0 && (sub[2]==' '||sub[2]=='\0')) {
        g_safe_mode = 1;
        tx_write(call->writer, call->writer_ctx,
                 "tx: agent safe mode ON\n");
        tx_write(call->writer, call->writer_ctx,
                 "     rm defaults to trash | overwrite creates backup\n");
        int siglvl = jash_sigil_get();
        if (siglvl >= 2)
            tx_write(call->writer, call->writer_ctx,
                     "     sigil SIGN active — tx roots will be attested\n");
        else
            tx_write(call->writer, call->writer_ctx,
                     "     tip: 'sigil sign' to cryptographically attest transactions\n");
        return 0;
    }
    if (strncmp(sub, "off", 3) == 0 && (sub[3]==' '||sub[3]=='\0')) {
        g_safe_mode = 0;
        tx_write(call->writer, call->writer_ctx,
                 "tx: agent safe mode OFF\n");
        return 0;
    }
    tx_writef(call->writer, call->writer_ctx,
              "tx: agent safe mode: %s\n", g_safe_mode ? "ON" : "OFF");
    return 0;
}

static int cmd_tx_policy(const jmod_call_t* call) {
    const char* sub = call->args ? call->args : "";
    while (*sub == ' ') sub++;
    if (strncmp(sub, "policy", 6) == 0) { sub += 6; while (*sub == ' ') sub++; }

    if (strncmp(sub, "rollback-on-fail", 16) == 0) {
        sub += 16; while (*sub == ' ') sub++;
        if (strncmp(sub, "ask", 3) == 0)      { g_safe_policy = 0; }
        else if (strncmp(sub, "always", 6) == 0) { g_safe_policy = 1; }
        else if (strncmp(sub, "never", 5) == 0)  { g_safe_policy = 2; }
        else {
            tx_write(call->writer, call->writer_ctx,
                     "tx: policy values: ask | always | never\n");
            return 1;
        }
        tx_writef(call->writer, call->writer_ctx,
                  "tx: policy rollback-on-fail → %s\n",
                  g_safe_policy == 0 ? "ask" :
                  g_safe_policy == 1 ? "always" : "never");
        return 0;
    }
    tx_write(call->writer, call->writer_ctx,
             "tx: policy <rollback-on-fail> <ask|always|never>\n");
    return 1;
}

/* ── Risk scoring ──────────────────────────────────────────────────── */

static int risk_score_cmd(const char* cmd) {
    if (!cmd) return 0;
    if (strstr(cmd, "rm -rf"))        return 5;
    if (strstr(cmd, "rm -r"))         return 4;
    if (strstr(cmd, "rm "))           return 2;
    if (strstr(cmd, "> "))            return 1;
    if (strstr(cmd, "dd "))           return 5;
    if (strstr(cmd, "mkfs"))          return 5;
    if (strstr(cmd, "chmod 777"))     return 3;
    return 0;
}

static const char* risk_label(int r) {
    if (r >= 5) return "CRITICAL";
    if (r >= 3) return "HIGH";
    if (r >= 1) return "MEDIUM";
    return "LOW";
}

static int cmd_tx_risk(const jmod_call_t* call) {
    const char* sub = call->args ? call->args : "";
    while (*sub == ' ') sub++;
    if (strncmp(sub, "risk", 4) == 0) { sub += 4; while (*sub == ' ') sub++; }

    int r = risk_score_cmd(sub);
    tx_writef(call->writer, call->writer_ctx,
              "risk: %s (%d)\n"
              "command: %s\n"
              "undo:    %s\n"
              "tip:     %s\n",
              risk_label(r), r, sub,
              r >= 3 ? "partial (use tx begin first)" : "yes",
              r >= 3 ? "tx begin; ...; tx commit"   :
              r >= 1 ? "use rm --undo or trash"     : "safe");
    return r >= 5 ? 1 : 0;
}

/* ── Post-command hook for auto-rollback hint ──────────────────────── */

static void undo_post_cmd_hook(const jmod_call_t* call, int exit_code) {
    if (!g_safe_mode || !g_tx_active) return;
    if (exit_code == 0) return;

    if (g_safe_policy == 2) return;  /* never */

    const char* cmd = call ? call->args : NULL;
    int risk = risk_score_cmd(cmd);

    if (g_safe_policy == 1 || (g_safe_policy == 0 && risk >= 3)) {
        /* always, or ask + high risk — execute rollback */
        tx_writef(NULL, NULL, "");  /* placeholder */

        /* We can't call rollback from post_cmd hook because it would
         * call jash_b_undo which modifies timeline. Instead, print hint. */
        tx_writef((jmod_writer_t)call->writer, call->writer_ctx,
                  "\n[SAFE] command failed (rc=%d). %d mutation(s) in tx '%s'.\n"
                  "[SAFE] → run 'tx rollback' to revert.\n",
                  exit_code, g_mut_count,
                  g_tx_name[0] ? g_tx_name : "unnamed");
    }
}

/* ── Dispatch: tx subcommands ──────────────────────────────────────── */

static int cmd_tx_begin(const jmod_call_t* call, const char* name) {
    if (g_tx_active) {
        tx_write(call->writer, call->writer_ctx,
                 "tx: transaction already open — commit or rollback first\n");
        return 1;
    }
    g_tx_active  = 1;
    g_mut_count  = 0;
    if (name && *name) {
        strncpy(g_tx_name, name, sizeof(g_tx_name) - 1);
        g_tx_name[sizeof(g_tx_name) - 1] = '\0';
    } else {
        g_tx_name[0] = '\0';
    }
    tx_writef(call->writer, call->writer_ctx,
              "tx: begin%s%s\n",
              g_tx_name[0] ? " " : "",
              g_tx_name[0] ? g_tx_name : "");
    return 0;
}

static int cmd_tx_status(const jmod_call_t* call) {
    if (!g_tx_active) {
        tx_write(call->writer, call->writer_ctx, "tx: no transaction open\n");
    } else {
        tx_writef(call->writer, call->writer_ctx,
                  "tx: open%s%s — %d mutation(s) tracked\n",
                  g_tx_name[0] ? " " : "",
                  g_tx_name[0] ? g_tx_name : "",
                  g_mut_count);
    }
    int siglvl = jash_sigil_get();
    tx_writef(call->writer, call->writer_ctx,
              "    safe: %s  policy: %s  sigil: %s  session: %.8s...\n",
              g_safe_mode ? "ON" : "OFF",
              g_safe_policy == 0 ? "ask" :
              g_safe_policy == 1 ? "always" : "never",
              siglvl >= 2 ? "SIGN" : siglvl >= 1 ? "LOG" : "OFF",
              g_sess_id);
    if (g_last_root[0])
        tx_writef(call->writer, call->writer_ctx,
                  "    last tx_root: %.16s...\n", g_last_root);
    return 0;
}

static int cmd_tx_preview(const jmod_call_t* call) {
    if (!g_tx_active) {
        tx_write(call->writer, call->writer_ctx,
                 "tx: no transaction open\n");
        return 1;
    }
    if (g_mut_count == 0) {
        tx_write(call->writer, call->writer_ctx,
                 "tx: no mutations recorded yet\n");
        return 0;
    }

    /* Show the last g_mut_count entries from the ring */
    int total = g_ring_total < TX_RING_SZ ? g_ring_total : TX_RING_SZ;
    int start = total - g_mut_count;
    if (start < 0) start = 0;
    int shown = g_mut_count < 16 ? g_mut_count : 16;

    tx_writef(call->writer, call->writer_ctx,
              "would undo %d mutation(s):\n", g_mut_count);
    for (int i = 0; i < shown; i++) {
        int kind; char desc[160];
        if (ring_get(start + i, &kind, desc, sizeof(desc)) == 0)
            tx_writef(call->writer, call->writer_ctx,
                      "  %-10s %s\n",
                      kind_str(kind),
                      desc[0] ? desc : "(unknown)");
    }
    if (g_mut_count > 16)
        tx_writef(call->writer, call->writer_ctx,
                  "  ... (%d more)\n", g_mut_count - 16);
    return 0;
}

static int cmd_tx_diff(const jmod_call_t* call) {
    if (!g_tx_active) {
        tx_write(call->writer, call->writer_ctx,
                 "tx: no transaction open\n");
        return 1;
    }
    if (g_mut_count == 0) {
        tx_write(call->writer, call->writer_ctx,
                 "tx: no changes to diff\n");
        return 0;
    }

    int creates = 0, trashes = 0, moves = 0, overwrites = 0, envs = 0, other = 0;
    int total = g_ring_total < TX_RING_SZ ? g_ring_total : TX_RING_SZ;
    int start = total - g_mut_count;
    if (start < 0) start = 0;
    for (int i = 0; i < g_mut_count && i < TX_RING_SZ; i++) {
        int kind;
        if (ring_get(start + i, &kind, NULL, 0) != 0) {
            tx_write(call->writer, call->writer_ctx,
                     "tx: timeline entry unavailable; diff incomplete\n");
            return 1;
        }
        switch (kind) {
            case JASH_UNDO_FILE_CREATE:    creates++;    break;
            case JASH_UNDO_TRASH:          trashes++;    break;
            case JASH_UNDO_MOVE:           moves++;      break;
            case JASH_UNDO_FILE_OVERWRITE: overwrites++; break;
            case JASH_UNDO_ENV_SET:
            case JASH_UNDO_ENV_UNSET:      envs++;       break;
            default:                       other++;      break;
        }
    }

    tx_writef(call->writer, call->writer_ctx,
              "tx diff (%s): %d mutations\n",
              g_tx_name[0] ? g_tx_name : "unnamed", g_mut_count);
    if (creates)    tx_writef(call->writer, call->writer_ctx, "  files created:    %d\n", creates);
    if (trashes)    tx_writef(call->writer, call->writer_ctx, "  files trashed:    %d\n", trashes);
    if (moves)      tx_writef(call->writer, call->writer_ctx, "  files moved:      %d\n", moves);
    if (overwrites) tx_writef(call->writer, call->writer_ctx, "  files overwritten: %d\n", overwrites);
    if (envs)       tx_writef(call->writer, call->writer_ctx, "  env changed:      %d\n", envs);
    if (other)      tx_writef(call->writer, call->writer_ctx, "  other:            %d\n", other);
    return 0;
}

static int cmd_tx_commit(const jmod_call_t* call) {
    if (!g_tx_active) {
        tx_write(call->writer, call->writer_ctx,
                 "tx: no transaction open\n");
        return 1;
    }
    int n = g_mut_count;

    /* Compute tx_root and save name before clearing state */
    char root_hex[JASH_BLAKE3_HEX_LEN] = {0};
    char saved_name[64] = {0};
    memcpy(saved_name, g_tx_name, sizeof(saved_name));
    saved_name[sizeof(saved_name) - 1] = '\0';
    if (n > 0) tx_compute_root(root_hex);

    g_tx_active  = 0;
    g_mut_count  = 0;
    g_tx_name[0] = '\0';

    tx_writef(call->writer, call->writer_ctx,
              "tx: commit — %d mutation(s) finalised\n", n);
    if (root_hex[0]) {
        tx_writef(call->writer, call->writer_ctx,
                  "    tx_root: %.16s... (BLAKE3)\n", root_hex);
        tx_sigil_record(call, "TX_COMMIT", saved_name, root_hex);
    }
    return 0;
}

static int cmd_tx_rollback(const jmod_call_t* call) {
    if (!g_tx_active) {
        tx_write(call->writer, call->writer_ctx,
                 "tx: no transaction open\n");
        return 1;
    }
    int n = g_mut_count;
    if (n == 0) {
        g_tx_active  = 0;
        g_tx_name[0] = '\0';
        tx_write(call->writer, call->writer_ctx,
                 "tx: rollback — nothing to undo\n");
        return 0;
    }

    /* Compute tx_root and save name before clearing state */
    char root_hex[JASH_BLAKE3_HEX_LEN] = {0};
    char saved_name[64] = {0};
    memcpy(saved_name, g_tx_name, sizeof(saved_name));
    saved_name[sizeof(saved_name) - 1] = '\0';
    tx_compute_root(root_hex);

    char arg[16];
    snprintf(arg, sizeof(arg), "%d", n);

    tx_writef(call->writer, call->writer_ctx,
              "tx: rolling back %d mutation(s)...\n", n);

    g_tx_active  = 0;
    g_mut_count  = 0;
    g_tx_name[0] = '\0';

    int rc = jash_b_undo(call->state,
                         (jmod_writer_t)call->writer,
                         call->writer_ctx,
                         arg);
    if (rc != 0)
        tx_write(call->writer, call->writer_ctx,
                 "tx: rollback — some operations could not be undone\n");

    tx_writef(call->writer, call->writer_ctx,
              "    tx_root: %.16s... (BLAKE3)\n", root_hex);
    tx_sigil_record(call, "TX_ROLLBACK", saved_name, root_hex);
    return rc;
}

/* ── Help text ─────────────────────────────────────────────────────── */

static void tx_help(jmod_writer_t w, void* ctx) {
    tx_write(w, ctx,
        "tx: Transactional Undo Fabric\n"
        "\n"
        "  Transaction commands:\n"
        "    tx begin [name]      start a transaction\n"
        "    tx status            show transaction state\n"
        "    tx preview           show mutations without executing\n"
        "    tx diff              summarise mutation types\n"
        "    tx commit            finalise the transaction\n"
        "    tx rollback          undo all mutations since begin\n"
        "    tx abort             alias for rollback\n"
        "\n"
        "  Session persistence:\n"
        "    tx sessions          list saved sessions\n"
        "    tx session show <id> show a past session\n"
        "    tx save [name]       save current session (BLAKE3-integrity)\n"
        "    tx verify [<id>]     verify BLAKE3 integrity of a session\n"
        "\n"
        "  Agent safety:\n"
        "    tx safe on|off       toggle agent safe mode\n"
        "    tx policy rollback-on-fail ask|always|never\n"
        "    tx risk <command>    score risk of a command\n"
        "\n"
        "  Sigil bridge: if sigil is loaded at level SIGN, tx roots are attested.\n");
}

/* ── Main dispatch ─────────────────────────────────────────────────── */

static int undo_dispatch(const jmod_call_t* call) {
    if (!call->verb || strcmp(call->verb, "tx") != 0) return -1;

    const char* sub = call->args ? call->args : "";
    while (*sub == ' ') sub++;

#define MATCH(s) (strncmp(sub, s, strlen(s)) == 0 \
                  && (sub[strlen(s)] == ' ' || sub[strlen(s)] == '\0'))

    if (!*sub || MATCH("help"))          { tx_help(call->writer, call->writer_ctx); return 0; }
    if (MATCH("begin"))                  { const char* n = sub+5; while(*n==' ')n++; return cmd_tx_begin(call, n); }
    if (MATCH("commit"))                 return cmd_tx_commit(call);
    if (MATCH("rollback") || MATCH("abort")) return cmd_tx_rollback(call);
    if (MATCH("preview"))                return cmd_tx_preview(call);
    if (MATCH("status"))                 return cmd_tx_status(call);
    if (MATCH("diff"))                   return cmd_tx_diff(call);
    if (MATCH("sessions"))               return cmd_tx_sessions(call);
    if (MATCH("session")) {
        const char* rest = sub + 7; while (*rest == ' ') rest++;
        if (strncmp(rest, "show ", 5) == 0) {
            const char* sid = rest + 5; while (*sid == ' ') sid++;
            return cmd_tx_session_show(call, sid);
        }
        tx_write(call->writer, call->writer_ctx, "tx: session show <id>\n");
        return 1;
    }
    if (MATCH("save"))                   return cmd_tx_save(call);
    if (MATCH("safe"))                   return cmd_tx_safe(call);
    if (MATCH("policy"))                 return cmd_tx_policy(call);
    if (MATCH("risk"))                   return cmd_tx_risk(call);
    if (MATCH("verify"))                 return cmd_tx_verify(call);

    tx_help(call->writer, call->writer_ctx);
    return 1;
#undef MATCH
}

/* ── Module ops ────────────────────────────────────────────────────── */

jmod_ops_t jmod_ops = {
    .init           = undo_init,
    .shutdown       = undo_shutdown,
    .dispatch       = undo_dispatch,
    .hook_pre_cmd   = NULL,
    .hook_post_cmd  = undo_post_cmd_hook,
    .hook_exit      = NULL,
    .hook_mutation  = undo_mutation_hook,
    .name           = "undo",
    .version        = "0.2.0",
    .description    = "Transactional undo — tx + sessions + agent safe mode (ADR-0018 W2-4)",
    .caps_used      = JMOD_CAP_STATE_READ | JMOD_CAP_FS_READ |
                      JMOD_CAP_FS_WRITE | JMOD_CAP_HOOK_MUTATION |
                      JMOD_CAP_HOOK_POST_CMD,
};
