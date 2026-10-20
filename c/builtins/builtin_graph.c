// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — builtins/graph — living graph: the repo knowledge map that learns
// from use. F E R M I ∞ H A R T
//
// ADR-0053. A code graph says what the code IS; a living graph says what the
// code is AND where agents trip:
//
//   build   — walk .c/.h, parse with the native parser (ADR-0046), nodes
//             F/S/E/T + module nodes M, edges CALLS (EXTRACTED when the
//             callee has one definition repo-wide, INFERRED when resolved by
//             name across files), CONTAINS (module→node), INCLUDES
//             (module→module). Incremental: only files whose mtime+size
//             changed are re-parsed; the delta is DECLARED, never silent.
//   explain / path / god-nodes / communities / stats
//           — verbs over the PERSISTED graph: each `jash -c` is a new
//             process, so the graph lives in ~/.jash/graph-<repokey>.tsv
//             (flock + tmp/rename, same discipline as gap/mesh).
//   hot     — the join nobody else does: graph × attested chain. Pain =
//             touches + 3×fails per file, cross-referenced with the gap
//             queue (fleet demand). God nodes by pain, not by degree.
//
// Confidence is declared on every edge: E = EXTRACTED (the parser read it in
// the source), I = INFERRED (name resolution across files). Never blended.
//
// --budget N caps any verb's output with an honest truncation footer (W5);
// --json is stable on every verb.

#define _GNU_SOURCE
#include "builtin.h"
#include <core/agent_json.h>
#include <core/structured_output.h>
#include <core/output_budget.h>
#include <core/adopt.h>        /* jash_repo_key — per-repo graph (ADR-0051 W6) */
#include <module/sigil.h>      /* jash_sigil_attest */
#include <graph/jash_graph.h>  /* JGGraph type (unused — parser takes NULL) */
#include <graph/jash_parse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <dirent.h>
#include <ctype.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>
#include <limits.h>     /* PATH_MAX — realpath()'s mandated buffer size */

/* ── store ───────────────────────────────────────────────────────────────── */

#define LG_CAP_NODES 32768
#define LG_CAP_EDGES 131072
#define LG_CAP_FILES 8192
#define LG_HNAME     (1 << 15)
#define LG_HFILE     (1 << 13)
#define LG_HEDGE     (1 << 17)
#define LG_CAP_CALLS 65536
#define LG_CAP_INCS  16384

typedef struct { char name[128]; int fidx; long line; char type; int community; } lg_node_t;
typedef struct { unsigned src, dst; char rel; char conf; } lg_edge_t;
typedef struct { char path[384]; long mtime; long size; } lg_file_t;

/* rel:  C=calls  K=contains  I=includes      conf: E=EXTRACTED I=INFERRED */
/* type: F=func   S=struct/union  E=enum  T=typedef  M=module(file)          */

static lg_node_t *N;  static int nN;
static lg_edge_t *E;  static int nE;
static lg_file_t *F;  static int nF;

static lg_node_t *oN; static int onN;
static lg_edge_t *oE; static int onE;
static lg_file_t *oF; static int onF;

static int *nhash, *nnext;          /* name  → node chain (LG_HNAME)   */
static int *fhash, *fnext;          /* path  → file chain (LG_HFILE)   */
static unsigned long long *ehash;   /* edge dedup set    (LG_HEDGE)    */
static int *eidx;                   /* ehash slot → edge index         */
static long *a_mt, *a_sz;           /* merge: A-side per-file stats snapshot */
static int *adj_off, *adj_dst, *adj_edge, *deg;   /* CSR adjacency      */
static int *queue, *prev_node, *prev_edge;        /* BFS                */
static char *visited;
static int *tally, *lg_seen, *comm_rank;             /* label propagation  */
static int seen_stamp;
static int *fmodule;                /* fidx  → module node idx         */
static int *fstamp;                 /* hot: per-line dedup             */
static int *bhash, *bnext;          /* hot: basename → file chain      */
static int *old2new;                /* build: old node idx → new idx   */
static int *ofile_status;           /* build: 0 gone, 1 reused, 2 changed */
static int *ofile_newmap;           /* build: old fidx → new fidx      */

typedef struct { unsigned caller; char name[128]; } lg_call_t;
typedef struct { unsigned src;    char inc[384];  } lg_inc_t;
static lg_call_t *calls; static int ncalls;
static lg_inc_t  *incs;  static int nincs;

static int lg_truncated;            /* a cap was hit — declared in output */

static int lg_allocation_ready;
static int lg_alloc(void) {
    if (N) return lg_allocation_ready;
    N = calloc(LG_CAP_NODES, sizeof *N);
    E = calloc(LG_CAP_EDGES, sizeof *E);
    F = calloc(LG_CAP_FILES, sizeof *F);
    oN = calloc(LG_CAP_NODES, sizeof *oN);
    oE = calloc(LG_CAP_EDGES, sizeof *oE);
    oF = calloc(LG_CAP_FILES, sizeof *oF);
    nhash = calloc(LG_HNAME, sizeof(int));
    nnext = calloc(LG_CAP_NODES, sizeof(int));
    fhash = calloc(LG_HFILE, sizeof(int));
    fnext = calloc(LG_CAP_FILES, sizeof(int));
    ehash = calloc(LG_HEDGE, sizeof(unsigned long long));
    eidx = calloc(LG_HEDGE, sizeof(int));
    a_mt = calloc(LG_CAP_FILES, sizeof(long));
    a_sz = calloc(LG_CAP_FILES, sizeof(long));
    adj_off = calloc(LG_CAP_NODES + 1, sizeof(int));
    adj_dst = calloc(2 * LG_CAP_EDGES, sizeof(int));
    adj_edge = calloc(2 * LG_CAP_EDGES, sizeof(int));
    deg = calloc(LG_CAP_NODES, sizeof(int));
    queue = calloc(LG_CAP_NODES, sizeof(int));
    prev_node = calloc(LG_CAP_NODES, sizeof(int));
    prev_edge = calloc(LG_CAP_NODES, sizeof(int));
    visited = calloc(LG_CAP_NODES, 1);
    tally = calloc(LG_CAP_NODES, sizeof(int));
    lg_seen = calloc(LG_CAP_NODES, sizeof(int));
    comm_rank = calloc(LG_CAP_NODES, sizeof(int));
    fmodule = calloc(LG_CAP_FILES, sizeof(int));
    fstamp = calloc(LG_CAP_FILES, sizeof(int));
    bhash = calloc(LG_HFILE, sizeof(int));
    bnext = calloc(LG_CAP_FILES, sizeof(int));
    old2new = calloc(LG_CAP_NODES, sizeof(int));
    ofile_status = calloc(LG_CAP_FILES, sizeof(int));
    ofile_newmap = calloc(LG_CAP_FILES, sizeof(int));
    calls = calloc(LG_CAP_CALLS, sizeof *calls);
    incs = calloc(LG_CAP_INCS, sizeof *incs);
    lg_allocation_ready = N && E && F && oN && oE && oF && nhash && nnext && fhash && fnext &&
           ehash && eidx && a_mt && a_sz && adj_off && adj_dst && adj_edge && deg &&
           queue && prev_node &&
           prev_edge && visited && tally && lg_seen && comm_rank && fmodule &&
           fstamp && bhash && bnext && old2new && ofile_status && ofile_newmap &&
           calls && incs;
    return lg_allocation_ready;
}

/* FNV-1a over a NUL-terminated string. */
static unsigned long lg_fnv(const char *s) {
    unsigned long h = 1469598103934665603ULL;
    for (; s && *s; s++) { h ^= (unsigned char)*s; h *= 1099511628211ULL; }
    return h;
}

static void lg_reset_hashes(void) {
    for (int i = 0; i < LG_HNAME; i++) nhash[i] = -1;
    for (int i = 0; i < LG_HFILE; i++) fhash[i] = -1;
    memset(ehash, 0, LG_HEDGE * sizeof(unsigned long long));
    memset(eidx, 0xFF, LG_HEDGE * sizeof(int));
}

static int lg_file_find(const char *path) {
    for (int i = fhash[lg_fnv(path) & (LG_HFILE - 1)]; i >= 0; i = fnext[i])
        if (!strcmp(F[i].path, path)) return i;
    return -1;
}

static int lg_file_add(const char *path) {
    int idx = lg_file_find(path);
    if (idx >= 0) return idx;
    if (nF >= LG_CAP_FILES) { lg_truncated = 1; return -1; }
    idx = nF++;
    memset(&F[idx], 0, sizeof F[idx]);
    snprintf(F[idx].path, sizeof F[idx].path, "%s", path);
    unsigned b = lg_fnv(path) & (LG_HFILE - 1);
    fnext[idx] = fhash[b]; fhash[b] = idx;
    return idx;
}

/* dedup by (name,type,fidx) — a node is its identity, not its position */
static int lg_node_add(const char *name, int fidx, long line, char type) {
    unsigned b = lg_fnv(name) & (LG_HNAME - 1);
    for (int i = nhash[b]; i >= 0; i = nnext[i])
        if (N[i].fidx == fidx && N[i].type == type && !strcmp(N[i].name, name))
            return i;
    if (nN >= LG_CAP_NODES) { lg_truncated = 1; return -1; }
    int idx = nN++;
    memset(&N[idx], 0, sizeof N[idx]);
    snprintf(N[idx].name, sizeof N[idx].name, "%s", name);
    N[idx].fidx = fidx;
    N[idx].line = line;
    N[idx].type = type;
    N[idx].community = idx;
    nnext[idx] = nhash[b]; nhash[b] = idx;
    return idx;
}

static unsigned lg_edge_slot(int src, int dst, char rel) {
    unsigned long long code = rel == 'C' ? 0 : rel == 'K' ? 1 : 2;
    unsigned long long key = ((unsigned long long)src << 34) |
                             ((unsigned long long)dst << 4) | code;
    return (key * 11400714819323198485ULL) & (LG_HEDGE - 1);
}

static int lg_edge_add(int src, int dst, char rel, char conf) {
    if (src < 0 || dst < 0) return -1;
    unsigned long long code = rel == 'C' ? 0 : rel == 'K' ? 1 : 2;
    unsigned long long key = ((unsigned long long)src << 34) |
                             ((unsigned long long)dst << 4) | code;
    unsigned b = lg_edge_slot(src, dst, rel);
    for (unsigned p = 0; p < LG_HEDGE; p++) {
        unsigned s = (b + p) & (LG_HEDGE - 1);
        if (!ehash[s]) {
            if (nE >= LG_CAP_EDGES) { lg_truncated = 1; return -1; }
            ehash[s] = key + 1; eidx[s] = nE; break;
        }
        if (ehash[s] == key + 1) return 0;   /* dedup (src,dst,rel) */
    }
    if (nE >= LG_CAP_EDGES) { lg_truncated = 1; return -1; }
    E[nE].src = src; E[nE].dst = dst; E[nE].rel = rel; E[nE].conf = conf;
    nE++;
    return 1;
}

/* W7 merge variant: same dedup, but a confidence conflict resolves to
 * EXTRACTED — what was READ from source always beats what was deduced */
static int lg_edge_merge_add(int src, int dst, char rel, char conf) {
    if (src < 0 || dst < 0) return 0;
    unsigned b = lg_edge_slot(src, dst, rel);
    for (unsigned p = 0; p < LG_HEDGE; p++) {
        unsigned s = (b + p) & (LG_HEDGE - 1);
        if (!ehash[s]) break;
        unsigned long long code = rel == 'C' ? 0 : rel == 'K' ? 1 : 2;
        unsigned long long key = ((unsigned long long)src << 34) |
                                 ((unsigned long long)dst << 4) | code;
        if (ehash[s] == key + 1) {
            int ei = eidx[s];
            if (ei >= 0 && ei < nE && E[ei].conf == 'I' && conf == 'E') {
                E[ei].conf = 'E';
                return 2;   /* upgraded INFERRED → EXTRACTED */
            }
            return 0;
        }
    }
    return lg_edge_add(src, dst, rel, conf) == 1 ? 1 : 0;
}

/* undirected CSR adjacency over ALL edges (explain/path/degree/hot) */
static void lg_build_adj(void) {
    memset(deg, 0, nN * sizeof(int));
    for (int i = 0; i < nE; i++) { deg[E[i].src]++; deg[E[i].dst]++; }
    adj_off[0] = 0;
    for (int i = 0; i < nN; i++) adj_off[i + 1] = adj_off[i] + deg[i];
    int *cur = calloc(nN > 0 ? nN : 1, sizeof(int));
    if (!cur) { lg_truncated = 1; return; }
    for (int i = 0; i < nE; i++) {
        int s = E[i].src, d = E[i].dst;
        int ps = adj_off[s] + cur[s]++;
        int pd = adj_off[d] + cur[d]++;
        if (ps < 2 * LG_CAP_EDGES) { adj_dst[ps] = d; adj_edge[ps] = i; }
        if (pd < 2 * LG_CAP_EDGES) { adj_dst[pd] = s; adj_edge[pd] = i; }
    }
    free(cur);
}

/* ── paths, lock, output ───────────────────────────────────────────────────── */

static int lg_graph_path(char *b, size_t cap) {
    const char *h = getenv("HOME");
    if (!h || !*h) return 0;
    const char *k = jash_repo_key();
    if (k && k[0] != '-')
        snprintf(b, cap, "%s/.jash/graph-%s.tsv", h, k);
    else
        snprintf(b, cap, "%s/.jash/graph.tsv", h);
    return 1;
}

static int lg_lock(void) {
    const char *h = getenv("HOME");
    if (!h || !*h) return -1;
    char d[600], lp[700];
    snprintf(d, sizeof d, "%s/.jash", h);        mkdir(d, 0700);
    const char *k = jash_repo_key();
    if (k && k[0] != '-')
        snprintf(lp, sizeof lp, "%s/.jash/graph-%s.lock", h, k);
    else
        snprintf(lp, sizeof lp, "%s/.jash/graph.lock", h);
    int fd = open(lp, O_CREAT | O_RDWR, 0600);
    if (fd < 0) return -1;
    if (flock(fd, LOCK_EX) != 0) { close(fd); return -1; }
    return fd;
}
static void lg_unlock(int fd) { if (fd >= 0) { flock(fd, LOCK_UN); close(fd); } }

/* ── W7: mesh fabric ─────────────────────────────────────────────────────────
 * Shared graph lives in ~/.jash/mesh/graph-<repokey>.tsv under the SAME lock
 * as claims/tasks (~/.jash/mesh/.lock) — one critical section for the whole
 * fabric. The mesh dir must already exist (mesh active); push/sync are
 * explicit verbs, so an inactive mesh is an honest error, never a silent skip. */

static int lg_mesh_active(void) {
    const char *h = getenv("HOME");
    if (!h || !*h) return 0;
    char md[600];
    snprintf(md, sizeof md, "%s/.jash/mesh", h);
    struct stat st;
    return stat(md, &st) == 0 && S_ISDIR(st.st_mode);
}

static int lg_mesh_graph_path(char *b, size_t cap) {
    const char *h = getenv("HOME");
    if (!h || !*h) return 0;
    const char *k = jash_repo_key();
    if (k && k[0] != '-')
        snprintf(b, cap, "%s/.jash/mesh/graph-%s.tsv", h, k);
    else
        snprintf(b, cap, "%s/.jash/mesh/graph.tsv", h);
    return 1;
}

static int lg_mesh_lock(void) {
    const char *h = getenv("HOME");
    if (!h || !*h) return -1;
    char lp[700];
    snprintf(lp, sizeof lp, "%s/.jash/mesh/.lock", h);
    int fd = open(lp, O_CREAT | O_RDWR, 0600);
    if (fd < 0) return -1;
    if (flock(fd, LOCK_EX) != 0) { close(fd); return -1; }
    return fd;
}

/* deterministic union of the fleet graph (oN/oE/oF) into the live store.
 * One logic, one place — push and sync both call this, so merge(A,B) and
 * merge(B,A) yield the same counts:
 *   files — union, max mtime (tie: max size)
 *   nodes — union by (name,type,file); conflict → the side whose file is
 *           NEWER (own registries; tie: bigger size, then A) wins the data
 *   edges — union by node-identity key; confidence conflict → EXTRACTED wins */
static int lg_merge_nodes_added, lg_merge_edges_added, lg_merge_upgraded;
static void lg_merge_fleet(void) {
    lg_merge_nodes_added = lg_merge_edges_added = lg_merge_upgraded = 0;

    /* snapshot A-side stats for files both sides have (step 1 max-merges
     * F[], which would erase the per-side values the conflict rule needs) */
    for (int j = 0; j < onF; j++) {
        int af = lg_file_find(oF[j].path);
        if (af >= 0) { a_mt[af] = F[af].mtime; a_sz[af] = F[af].size; fstamp[af] = 1; }
    }

    /* files */
    for (int j = 0; j < onF; j++) {
        int af = lg_file_find(oF[j].path);
        if (af < 0) {
            af = lg_file_add(oF[j].path);
            if (af < 0) continue;
            F[af].mtime = oF[j].mtime;
            F[af].size = oF[j].size;
        } else if (oF[j].mtime > F[af].mtime ||
                   (oF[j].mtime == F[af].mtime && oF[j].size > F[af].size)) {
            F[af].mtime = oF[j].mtime;
            F[af].size = oF[j].size;
        }
    }

    /* nodes */
    for (int j = 0; j < onN; j++) {
        int of = oN[j].fidx;
        if (of < 0 || of >= onF) continue;
        int af = lg_file_find(oF[of].path);
        if (af < 0) continue;
        unsigned b = lg_fnv(oN[j].name) & (LG_HNAME - 1);
        int found = -1;
        for (int i = nhash[b]; i >= 0; i = nnext[i])
            if (N[i].fidx == af && N[i].type == oN[j].type &&
                !strcmp(N[i].name, oN[j].name)) { found = i; break; }
        if (found < 0) {
            if (lg_node_add(oN[j].name, af, oN[j].line, oN[j].type) >= 0)
                lg_merge_nodes_added++;
            continue;
        }
        if (!fstamp[af]) continue;   /* file only on B → node was just added */
        long am = a_mt[af], as = a_sz[af];
        long bm = oF[of].mtime, bs = oF[of].size;
        if (bm > am || (bm == am && bs > as))
            N[found].line = oN[j].line;   /* B's version of the file is newer */
    }

    /* edges — endpoints re-resolved by node identity, never by index */
    for (int j = 0; j < onE; j++) {
        int os = (int)oE[j].src, od = (int)oE[j].dst;
        if (os >= onN || od >= onN) continue;
        int ofs = oN[os].fidx, ofd = oN[od].fidx;
        if (ofs < 0 || ofs >= onF || ofd < 0 || ofd >= onF) continue;
        int afs = lg_file_find(oF[ofs].path);
        int afd = lg_file_find(oF[ofd].path);
        if (afs < 0 || afd < 0) continue;
        int ns = -1, nd = -1;
        unsigned bs = lg_fnv(oN[os].name) & (LG_HNAME - 1);
        for (int i = nhash[bs]; i >= 0; i = nnext[i])
            if (N[i].fidx == afs && N[i].type == oN[os].type &&
                !strcmp(N[i].name, oN[os].name)) { ns = i; break; }
        unsigned bd = lg_fnv(oN[od].name) & (LG_HNAME - 1);
        for (int i = nhash[bd]; i >= 0; i = nnext[i])
            if (N[i].fidx == afd && N[i].type == oN[od].type &&
                !strcmp(N[i].name, oN[od].name)) { nd = i; break; }
        if (ns < 0 || nd < 0) continue;
        int r = lg_edge_merge_add(ns, nd, oE[j].rel, oE[j].conf);
        if (r == 1) lg_merge_edges_added++;
        else if (r == 2) lg_merge_upgraded++;
    }
}

/* TSV fields can't carry tab/newline — flatten to spaces. */
static void lg_flat(char *s) {
    for (; s && *s; s++) if (*s == '\t' || *s == '\n' || *s == '\r') *s = ' ';
}

static char lg_obuf[1 << 20];
static size_t lg_olen;
static size_t lg_required;
static int lg_json_mode, lg_output_status, lg_flushed;

static void lg_out(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));
static void lg_out(const char *fmt, ...) {
    size_t available = sizeof lg_obuf - lg_olen;
    va_list ap; va_start(ap, fmt);
    int r = vsnprintf(lg_obuf + lg_olen, available, fmt, ap);
    va_end(ap);
    if (r < 0) { lg_output_status = 125; return; }
    lg_required += (size_t)r;
    if (r > 0) lg_olen += (size_t)r < available ? (size_t)r : available - 1;
}

static void lg_flush(jash_writer_t w, void *ctx, long budget) {
    lg_flushed = 1;
    if (lg_json_mode) {
        long global_budget = jash_budget_limit();
        if (global_budget > 0 && (budget <= 0 || global_budget < budget))
            budget = global_budget;
        int rc = jash_structured_document(w, ctx, "graph", lg_obuf,
                                           lg_olen, lg_required, budget, 0);
        if (rc) lg_output_status = rc;
        lg_olen = 0; lg_required = 0; lg_obuf[0] = 0;
        return;
    }

    if (budget > 0 && (long)lg_olen > budget) {
        lg_obuf[budget] = 0;
        jash_writes(w, ctx, lg_obuf);
        jash_writef(w, ctx,
            "[graph] output capped at %ld bytes — refine with --top/--json\n", budget);
    } else {
        jash_writes(w, ctx, lg_obuf);
    }
    lg_olen = 0; lg_required = 0; lg_obuf[0] = 0;
}

static const char *lg_opt(char *a[], int n, const char *flag) {
    for (int i = 0; i < n - 1; i++) if (!strcmp(a[i], flag)) return a[i + 1];
    return NULL;
}
static int lg_has(char *a[], int n, const char *flag) {
    for (int i = 0; i < n; i++) if (!strcmp(a[i], flag)) return 1;
    return 0;
}
static long lg_budget(char *a[], int n) {
    const char *v = lg_opt(a, n, "--budget");
    return v ? atol(v) : 0;
}
static int lg_top(char *a[], int n, int dflt) {
    const char *v = lg_opt(a, n, "--top");
    int t = v ? atoi(v) : dflt;
    return t < 1 ? dflt : t;
}
/* which-th positional arg, skipping flags and the values of --budget/--top */
static const char *lg_pos(char *a[], int n, int which) {
    int found = 0;
    for (int i = 0; i < n; i++) {
        if (!strcmp(a[i], "--budget") || !strcmp(a[i], "--top")) { i++; continue; }
        if (a[i][0] == '-') continue;
        if (found++ == which) return a[i];
    }
    return NULL;
}

/* ── persistence ───────────────────────────────────────────────────────────── */

/* split on '\t' PRESERVING empty fields (strtok collapses them) */
static int lg_split(char *line, char *fld[], int max) {
    int nf = 0;
    char *s = line;
    while (nf < max && s) {
        fld[nf++] = s;
        char *t = strchr(s, '\t');
        s = t ? (*t = 0, t + 1) : NULL;
    }
    return nf;
}

static int lg_load_into(const char *path,
                        lg_node_t *aN, int *pnN, lg_edge_t *aE, int *pnE,
                        lg_file_t *aF, int *pnF) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    /* local path→fidx map: F lines are written before the N lines that
     * reference them */
    int *lhash = calloc(LG_HFILE, sizeof(int));
    int *lnext = calloc(LG_CAP_FILES, sizeof(int));
    if (!lhash || !lnext) { fclose(f); free(lhash); free(lnext); return 0; }
    for (int i = 0; i < LG_HFILE; i++) lhash[i] = -1;
    int nn = 0, ne = 0, nf = 0;
    char line[1600];
    while (fgets(line, sizeof line, f)) {
        char *nl = strchr(line, '\n'); if (nl) *nl = 0;
        if (!line[0]) continue;
        char *fld[8] = {0};
        int nfd = lg_split(line, fld, 8);
        if (line[0] == 'F' && nfd >= 4 && nf < LG_CAP_FILES) {
            lg_file_t *t = &aF[nf];
            memset(t, 0, sizeof *t);
            snprintf(t->path, sizeof t->path, "%s", fld[1]);
            t->mtime = atol(fld[2]);
            t->size = atol(fld[3]);
            unsigned b = lg_fnv(t->path) & (LG_HFILE - 1);
            lnext[nf] = lhash[b]; lhash[b] = nf;
            nf++;
        } else if (line[0] == 'N' && nfd >= 6 && nn < LG_CAP_NODES) {
            lg_node_t *t = &aN[nn];
            memset(t, 0, sizeof *t);
            snprintf(t->name, sizeof t->name, "%s", fld[1]);
            t->type = fld[2][0];
            t->fidx = -1;
            unsigned b = lg_fnv(fld[3]) & (LG_HFILE - 1);
            for (int i = lhash[b]; i >= 0; i = lnext[i])
                if (!strcmp(aF[i].path, fld[3])) { t->fidx = i; break; }
            if (t->fidx < 0) continue;
            t->line = atol(fld[4]);
            t->community = atoi(fld[5]);
            nn++;
        } else if (line[0] == 'E' && nfd >= 5 && ne < LG_CAP_EDGES) {
            aE[ne].src = (unsigned)atol(fld[1]);
            aE[ne].dst = (unsigned)atol(fld[2]);
            aE[ne].rel = fld[3][0];
            aE[ne].conf = fld[4][0];
            if (aE[ne].src < (unsigned)nn && aE[ne].dst < (unsigned)nn) ne++;
        }
    }
    fclose(f);
    free(lhash); free(lnext);
    *pnN = nn; *pnE = ne; *pnF = nf;
    return 1;
}

static int lg_load(void) {
    char gp[700];
    nN = nE = nF = 0;
    if (!lg_graph_path(gp, sizeof gp)) return 0;
    if (!lg_load_into(gp, N, &nN, E, &nE, F, &nF)) return 0;
    lg_reset_hashes();
    for (int i = 0; i < nF; i++) {
        unsigned b = lg_fnv(F[i].path) & (LG_HFILE - 1);
        fnext[i] = fhash[b]; fhash[b] = i;
    }
    for (int i = 0; i < nN; i++) {
        unsigned b = lg_fnv(N[i].name) & (LG_HNAME - 1);
        nnext[i] = nhash[b]; nhash[b] = i;
    }
    /* re-register edges in the dedup set — merge-add (W7) probes it */
    for (int i = 0; i < nE; i++) {
        unsigned long long code = E[i].rel == 'C' ? 0 : E[i].rel == 'K' ? 1 : 2;
        unsigned long long key = ((unsigned long long)E[i].src << 34) |
                                 ((unsigned long long)E[i].dst << 4) | code;
        unsigned b = lg_edge_slot((int)E[i].src, (int)E[i].dst, E[i].rel);
        for (unsigned p = 0; p < LG_HEDGE; p++) {
            unsigned s = (b + p) & (LG_HEDGE - 1);
            if (!ehash[s]) { ehash[s] = key + 1; eidx[s] = i; break; }
            if (ehash[s] == key + 1) break;
        }
    }
    return 1;
}

static void lg_store(const char *path) {
    char tmp[800];
    snprintf(tmp, sizeof tmp, "%s.tmp.%d", path, (int)getpid());
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    for (int i = 0; i < nF; i++) {
        lg_flat(F[i].path);
        fprintf(f, "F\t%s\t%ld\t%ld\n", F[i].path, F[i].mtime, F[i].size);
    }
    for (int i = 0; i < nN; i++) {
        char nm[128]; snprintf(nm, sizeof nm, "%s", N[i].name); lg_flat(nm);
        fprintf(f, "N\t%s\t%c\t%s\t%ld\t%d\n",
                nm, N[i].type, F[N[i].fidx].path, N[i].line, N[i].community);
    }
    for (int i = 0; i < nE; i++)
        fprintf(f, "E\t%u\t%u\t%c\t%c\n", E[i].src, E[i].dst, E[i].rel, E[i].conf);
    if (fclose(f) != 0 || rename(tmp, path) != 0) unlink(tmp);
}

/* ── text scanners (body brace-match, call scan, includes) ─────────────────── */

static long lg_skip_ws_comments(const char *s, long len, long i) {
    for (;;) {
        while (i < len && isspace((unsigned char)s[i])) i++;
        if (i + 1 < len && s[i] == '/' && s[i + 1] == '/') {
            while (i < len && s[i] != '\n') i++;
            continue;
        }
        if (i + 1 < len && s[i] == '/' && s[i + 1] == '*') {
            i += 2;
            while (i + 1 < len && !(s[i] == '*' && s[i + 1] == '/')) i++;
            if (i + 1 < len) i += 2; else i = len;
            continue;
        }
        return i;
    }
}

/* match the delimiter at s[i] to its close, skipping strings/chars/comments;
 * returns the index just AFTER the close, or -1 */
static long lg_match_delim(const char *s, long len, long i, char open, char close) {
    long depth = 0;
    while (i < len) {
        char c = s[i];
        if (c == '"') { i++; while (i < len && s[i] != '"') { if (s[i] == '\\') i++; i++; } i++; continue; }
        if (c == '\'') { i++; while (i < len && s[i] != '\'') { if (s[i] == '\\') i++; i++; } i++; continue; }
        if (c == '/' && i + 1 < len && s[i + 1] == '/') { while (i < len && s[i] != '\n') i++; continue; }
        if (c == '/' && i + 1 < len && s[i + 1] == '*') {
            i += 2;
            while (i + 1 < len && !(s[i] == '*' && s[i + 1] == '/')) i++;
            if (i + 1 < len) i += 2; else i = len;
            continue;
        }
        if (c == open) depth++;
        else if (c == close) { depth--; if (!depth) return i + 1; }
        i++;
    }
    return -1;
}

static int lg_is_ident_start(char c) { return isalpha((unsigned char)c) || c == '_'; }
static int lg_is_ident_char(char c)  { return isalnum((unsigned char)c) || c == '_'; }

/* not every ident( is a call — control keywords and casts are noise the
 * resolver would drop anyway; filtering here keeps the call list honest */
static const char *const LG_KEYWORDS[] = {
    "if", "for", "while", "switch", "return", "sizeof", "else", "do", "case",
    "goto", "default", "break", "continue", "typedef", "struct", "union",
    "enum", "static", "extern", "const", "volatile", "inline", "void", "int",
    "char", "long", "short", "float", "double", "signed", "unsigned",
    "register", "auto", "restrict", "alignof", "_Alignof", "defined", NULL
};
static int lg_is_keyword(const char *s, long len) {
    for (int i = 0; LG_KEYWORDS[i]; i++)
        if ((long)strlen(LG_KEYWORDS[i]) == len && !strncmp(LG_KEYWORDS[i], s, len))
            return 1;
    return 0;
}

/* scan a function body [b,e) for `ident(` call sites; append to the pending
 * call list, deduped per caller */
static void lg_scan_calls(const char *s, long b, long e, int caller) {
    int first = ncalls;
    long i = b;
    while (i < e) {
        char c = s[i];
        if (c == '"') { i++; while (i < e && s[i] != '"') { if (s[i] == '\\') i++; i++; } i++; continue; }
        if (c == '\'') { i++; while (i < e && s[i] != '\'') { if (s[i] == '\\') i++; i++; } i++; continue; }
        if (c == '#') { while (i < e && s[i] != '\n') i++; continue; }
        if (c == '/' && i + 1 < e && s[i + 1] == '/') { while (i < e && s[i] != '\n') i++; continue; }
        if (c == '/' && i + 1 < e && s[i + 1] == '*') {
            i += 2;
            while (i + 1 < e && !(s[i] == '*' && s[i + 1] == '/')) i++;
            if (i + 1 < e) i += 2; else i = e;
            continue;
        }
        if (lg_is_ident_start(c)) {
            long st = i;
            while (i < e && lg_is_ident_char(s[i])) i++;
            long nl = i - st;
            long k = lg_skip_ws_comments(s, e, i);
            if (k < e && s[k] == '(' && nl > 0 && nl < 120 &&
                !lg_is_keyword(s + st, nl)) {
                /* per-caller dedup */
                int dup = 0;
                for (int j = first; j < ncalls; j++)
                    if ((long)strlen(calls[j].name) == nl &&
                        !strncmp(calls[j].name, s + st, nl)) { dup = 1; break; }
                if (!dup && ncalls == LG_CAP_CALLS) { lg_truncated = 1; return; }
                if (!dup) {
                    calls[ncalls].caller = (unsigned)caller;
                    memcpy(calls[ncalls].name, s + st, nl);
                    calls[ncalls].name[nl] = 0;
                    ncalls++;
                }
            }
            continue;
        }
        i++;
    }
}

/* quoted includes only — system <...> headers are not repo files */
static void lg_scan_includes(const char *s, long len, int module_node) {
    long i = 0;
    while (i < len) {
        long ls = i;
        while (i < len && s[i] != '\n') i++;
        long le = i;
        if (i < len) i++;
        long p = ls;
        while (p < le && (s[p] == ' ' || s[p] == '\t')) p++;
        if (le - p < 9 || s[p] != '#') continue;
        p++;
        while (p < le && (s[p] == ' ' || s[p] == '\t')) p++;
        if (le - p < 7 || strncmp(s + p, "include", 7)) continue;
        p += 7;
        while (p < le && (s[p] == ' ' || s[p] == '\t')) p++;
        if (p >= le || s[p] != '"') continue;
        p++;
        long q = p;
        while (q < le && s[q] != '"') q++;
        if (q - p <= 0) continue;
        if (q - p >= 380) { lg_truncated = 1; return; }
        if (nincs >= LG_CAP_INCS) { lg_truncated = 1; return; }
        incs[nincs].src = (unsigned)module_node;
        memcpy(incs[nincs].inc, s + p, q - p);
        incs[nincs].inc[q - p] = 0;
        nincs++;
    }
}

/* lexical normalization of a relative path: collapse //, ., .. — returns 0
 * when the result escapes the root */
static int lg_normpath(const char *in, char *out, size_t cap) {
    char buf[800];
    snprintf(buf, sizeof buf, "%s", in);
    char *parts[128]; int np = 0;
    char *save = NULL;
    for (char *t = strtok_r(buf, "/", &save); t && np < 128; t = strtok_r(NULL, "/", &save)) {
        if (!strcmp(t, ".")) continue;
        if (!strcmp(t, "..")) { if (np == 0) return 0; np--; continue; }
        parts[np++] = t;
    }
    size_t o = 0;
    out[0] = 0;
    for (int i = 0; i < np; i++) {
        int r = snprintf(out + o, cap - o, "%s%s", i ? "/" : "", parts[i]);
        if (r < 0 || (size_t)r >= cap - o) return 0;
        o += (size_t)r;
    }
    return np > 0;
}

static long lg_line_of(const char *src, long upto) {
    long l = 1;
    for (long i = 0; i < upto; i++) if (src[i] == '\n') l++;
    return l;
}

/* ── extraction: one file → nodes + pending calls/includes ───────────────────
 * returns 1 when the file parsed (even if it yields zero nodes — a macros-only
 * header is legit), 0 when it could not be read/parsed (counted as skipped) */

static void lg_extract_nodes(int fidx, int midx, JSH_ParseContext* ctx) {
    const char *src = ctx->source_buffer;
    long slen = (long)ctx->source_len;

    for (uint32_t i = 0; i < ctx->node_count; i++) {
        JSH_Node *nd = &ctx->nodes[i];
        char type = 0;
        switch (nd->type) {
            case JSH_NODE_FUNCTION_DEFINITION:   type = 'F'; break;
            case JSH_NODE_STRUCT_DECLARATION:
            case JSH_NODE_UNION_DECLARATION:     type = 'S'; break;
            case JSH_NODE_ENUM_DECLARATION:      type = 'E'; break;
            case JSH_NODE_TYPEDEF_DECLARATION:   type = 'T'; break;
            default: break;
        }
        if (!type) continue;
        long nl = (long)nd->name.length;
        if (nl <= 0 || nl >= 120) continue;
        if ((long)nd->name.start + nl > slen) continue;
        char name[128];
        memcpy(name, src + nd->name.start, nl);
        name[nl] = 0;
        if (!lg_is_ident_start(name[0]) && type != 'M') continue;
        long line = nd->start_line ? (long)nd->start_line
                                   : lg_line_of(src, (long)nd->name.start);
        int nidx = lg_node_add(name, fidx, line, type);
        if (nidx < 0) continue;
        if (midx >= 0) lg_edge_add(midx, nidx, 'K', 'E');

        if (type != 'F') continue;
        /* locate the body in the source text: name ( params ) { body } */
        long p = lg_skip_ws_comments(src, slen, (long)nd->name.start + nl);
        if (p >= slen || src[p] != '(') continue;
        p = lg_match_delim(src, slen, p, '(', ')');
        if (p < 0) continue;
        p = lg_skip_ws_comments(src, slen, p);
        if (p >= slen || src[p] != '{') continue;
        long bend = lg_match_delim(src, slen, p, '{', '}');
        if (bend < 0) continue;
        lg_scan_calls(src, p + 1, bend - 1, nidx);
    }

    if (midx >= 0) lg_scan_includes(src, slen, midx);
}

static int lg_extract_file(const char *root, int fidx) {
    char abs[900];
    snprintf(abs, sizeof abs, "%s/%s", root, F[fidx].path);

    int midx = lg_node_add(F[fidx].path, fidx, 0, 'M');
    if (midx >= 0) fmodule[fidx] = midx;

    JSH_ParseContext ctx;
    if (jsh_parse_init(&ctx, NULL, abs) != 0) return 0;
    if (jsh_parse_file(&ctx, abs) != 0) { jsh_parse_destroy(&ctx); return 0; }

    lg_extract_nodes(fidx, midx, &ctx);
    jsh_parse_destroy(&ctx);
    return 1;
}

/* resolve the pending call list against the full node set.
 * one definition repo-wide → EXTRACTED; several (statics) → same-file else
 * first, INFERRED; unknown → dropped */
static void lg_resolve_calls(void) {
    for (int i = 0; i < ncalls; i++) {
        unsigned b = lg_fnv(calls[i].name) & (LG_HNAME - 1);
        int first = -1, same_file = -1, count = 0;
        int cfidx = N[calls[i].caller].fidx;
        for (int j = nhash[b]; j >= 0; j = nnext[j]) {
            if (N[j].type != 'F' || strcmp(N[j].name, calls[i].name)) continue;
            if (first < 0) first = j;
            if (N[j].fidx == cfidx) same_file = j;
            count++;
        }
        if (count == 0) continue;
        if (count == 1) {
            lg_edge_add(calls[i].caller, first, 'C', 'E');
        } else {
            lg_edge_add(calls[i].caller, same_file >= 0 ? same_file : first, 'C', 'I');
        }
    }
}

/* resolve pending includes against the file registry (module→module) */
static void lg_resolve_includes(void) {
    for (int i = 0; i < nincs; i++) {
        int sidx = (int)incs[i].src;
        const char *ifile = F[N[sidx].fidx].path;
        char cand[800], norm[800];
        int tf = -1;
        /* 1. relative to the including file's directory */
        const char *slash = strrchr(ifile, '/');
        if (slash) {
            snprintf(cand, sizeof cand, "%.*s/%s", (int)(slash - ifile), ifile, incs[i].inc);
            if (lg_normpath(cand, norm, sizeof norm)) tf = lg_file_find(norm);
        }
        /* 2. relative to the build root (include-path style) */
        if (tf < 0 && lg_normpath(incs[i].inc, norm, sizeof norm))
            tf = lg_file_find(norm);
        if (tf < 0 || fmodule[tf] < 0) continue;
        lg_edge_add(sidx, fmodule[tf], 'I', 'E');
    }
}

/* ── walk ──────────────────────────────────────────────────────────────────── */

static int lg_skip_dir(const char *name) {
    if (name[0] == '.') return 1;
    if (!strncmp(name, "build", 5)) return 1;
    if (!strcmp(name, "node_modules")) return 1;
    if (!strcmp(name, "scratchpad")) return 1;
    if (!strcmp(name, "Quarentena-codes")) return 1;
    return 0;
}

static void lg_walk(const char *abs, const char *rel) {
    DIR *d = opendir(abs);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d))) {
        const char *nm = de->d_name;
        if (!strcmp(nm, ".") || !strcmp(nm, "..")) continue;
        char ca[900], cr[800];
        snprintf(ca, sizeof ca, "%s/%s", abs, nm);
        snprintf(cr, sizeof cr, "%s%s%s", rel, rel[0] ? "/" : "", nm);
        struct stat st;
        if (stat(ca, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (!lg_skip_dir(nm)) lg_walk(ca, cr);
        } else if (S_ISREG(st.st_mode)) {
            size_t l = strlen(nm);
            if (l < 3 || (strcmp(nm + l - 2, ".c") && strcmp(nm + l - 2, ".h"))) continue;
            int fidx = lg_file_add(cr);
            if (fidx < 0) { closedir(d); return; }
            F[fidx].mtime = (long)st.st_mtime;
            F[fidx].size = (long)st.st_size;
        }
    }
    closedir(d);
}

static int lg_file_cmp(const void *a, const void *b) {
    return strcmp(((const lg_file_t *)a)->path, ((const lg_file_t *)b)->path);
}

/* ── communities: label propagation over calls+contains (undirected) ───────── */

static int lg_communities(void) {
    lg_build_adj();
    for (int i = 0; i < nN; i++) N[i].community = i;
    for (int iter = 0; iter < 10; iter++) {
        int changed = 0;
        for (int u = 0; u < nN; u++) {
            int b = adj_off[u], e = adj_off[u + 1];
            if (b == e) continue;
            seen_stamp++;
            for (int k = b; k < e; k++) {
                if (E[adj_edge[k]].rel == 'I') continue;   /* calls+contains only */
                int c = N[adj_dst[k]].community;
                if (lg_seen[c] != seen_stamp) { lg_seen[c] = seen_stamp; tally[c] = 0; }
                tally[c]++;
            }
            int best = N[u].community, bestc = 0;
            for (int k = b; k < e; k++) {
                if (E[adj_edge[k]].rel == 'I') continue;
                int c = N[adj_dst[k]].community;
                if (lg_seen[c] != seen_stamp) continue;
                lg_seen[c] = 0;   /* count each distinct label once */
                if (tally[c] > bestc || (tally[c] == bestc && c < best)) {
                    bestc = tally[c]; best = c;
                }
            }
            if (best != N[u].community) { N[u].community = best; changed = 1; }
        }
        if (!changed) break;
    }
    /* compact: rank labels by community size, biggest = 0 */
    for (int i = 0; i < nN; i++) { tally[i] = 0; comm_rank[i] = -1; }
    for (int i = 0; i < nN; i++) tally[N[i].community]++;
    typedef struct { int label, size; } lg_cl_t;
    lg_cl_t *cl = calloc(nN > 0 ? nN : 1, sizeof *cl);
    if (!cl) { lg_truncated = 1; return 0; }
    int nc = 0;
    for (int i = 0; i < nN; i++)
        if (tally[i] > 0) { cl[nc].label = i; cl[nc].size = tally[i]; nc++; }
    for (int i = 1; i < nc; i++) {   /* insertion sort, size desc */
        lg_cl_t t = cl[i]; int j = i - 1;
        while (j >= 0 && cl[j].size < t.size) { cl[j + 1] = cl[j]; j--; }
        cl[j + 1] = t;
    }
    for (int i = 0; i < nc; i++) comm_rank[cl[i].label] = i;
    for (int i = 0; i < nN; i++) N[i].community = comm_rank[N[i].community];
    free(cl);
    return nc;
}

/* ── lookup: exact → prefix → substring (modules are a second tier) ─────────── */

static int lg_lookup_tier(const char *q, int modules) {
    size_t ql = strlen(q);
    for (int i = 0; i < nN; i++) {
        if (!modules && N[i].type == 'M') continue;
        if (!strcmp(N[i].name, q)) return i;
    }
    for (int i = 0; i < nN; i++) {
        if (!modules && N[i].type == 'M') continue;
        if (!strncmp(N[i].name, q, ql)) return i;
    }
    for (int i = 0; i < nN; i++) {
        if (!modules && N[i].type == 'M') continue;
        if (strstr(N[i].name, q)) return i;
    }
    return -1;
}
static int lg_lookup(const char *q) {
    int i = lg_lookup_tier(q, 0);
    return i >= 0 ? i : lg_lookup_tier(q, 1);
}

static const char *lg_type_name(char t) {
    switch (t) {
        case 'F': return "func";
        case 'S': return "struct";
        case 'E': return "enum";
        case 'T': return "typedef";
        case 'M': return "module";
    }
    return "?";
}
static const char *lg_rel_out(char r) {
    return r == 'C' ? "calls" : r == 'K' ? "contains" : "includes";
}
static const char *lg_rel_in(char r) {
    return r == 'C' ? "calledby" : r == 'K' ? "contained-in" : "included-by";
}
static const char *lg_conf_name(char c) { return c == 'E' ? "EXTRACTED" : "INFERRED"; }

/* ── W1/W3: build ──────────────────────────────────────────────────────────── */

typedef struct { int reused, reparsed, gone, skipped, communities; } lg_build_observation_t;

static int lg_build_inventory(const char* root, int (*walk)(const char*, const char*),
                              int (*extract)(const char*, int), lg_build_observation_t* observed) {
    /* walk → F (sorted for a deterministic graph) */
    nN = nE = nF = 0; ncalls = 0; nincs = 0;
    lg_reset_hashes();
    if (walk) { if (walk(root, "")) return -1; }
    else lg_walk(root, "");
    qsort(F, nF, sizeof(lg_file_t), lg_file_cmp);
    for (int i = 0; i < LG_HFILE; i++) fhash[i] = -1;
    for (int i = 0; i < nF; i++) {
        unsigned b = lg_fnv(F[i].path) & (LG_HFILE - 1);
        fnext[i] = fhash[b]; fhash[b] = i;
    }

    /* old-file path → old fidx hash (bhash/bnext are free at build time) */
    for (int i = 0; i < LG_HFILE; i++) bhash[i] = -1;
    for (int j = 0; j < onF; j++) {
        unsigned b = lg_fnv(oF[j].path) & (LG_HFILE - 1);
        bnext[j] = bhash[b]; bhash[b] = j;
    }

    /* classify: which files are new/changed vs reusable */
    for (int i = 0; i < onF; i++) { ofile_status[i] = 0; ofile_newmap[i] = -1; }
    int reused = 0, reparsed = 0;
    char *want_parse = calloc(nF > 0 ? nF : 1, 1);
    if (!want_parse) return -1;
    for (int i = 0; i < nF; i++) {
        int found_old = -1;
        unsigned b = lg_fnv(F[i].path) & (LG_HFILE - 1);
        for (int j = bhash[b]; j >= 0; j = bnext[j])
            if (!strcmp(oF[j].path, F[i].path)) { found_old = j; break; }
        if (found_old >= 0) {
            ofile_newmap[found_old] = i;
            if (oF[found_old].mtime == F[i].mtime && oF[found_old].size == F[i].size) {
                ofile_status[found_old] = 1;
                reused++;
            } else {
                ofile_status[found_old] = 2;
                want_parse[i] = 1;
                reparsed++;
            }
        } else {
            want_parse[i] = 1;
            reparsed++;
        }
    }
    int gone = 0;
    for (int j = 0; j < onF; j++) if (ofile_status[j] == 0) gone++;

    /* old-node identity lookup (path+type+name) needs old file paths alive */
    for (int i = 0; i < onN; i++) old2new[i] = -1;
    for (int i = 0; i < LG_CAP_FILES; i++) fmodule[i] = -1;

    /* pass A: copy nodes of reused files, keeping their old identity */
    for (int i = 0; i < onN; i++) {
        int of = oN[i].fidx;
        if (of < 0 || of >= onF || ofile_status[of] != 1) continue;
        int nidx = lg_node_add(oN[i].name, ofile_newmap[of], oN[i].line, oN[i].type);
        old2new[i] = nidx;
        if (nidx >= 0 && oN[i].type == 'M') fmodule[ofile_newmap[of]] = nidx;
    }

    /* pass B: re-parse new/changed files — parse failures are counted and
     * DECLARED in the output, never silent */
    int skipped = 0;
    for (int i = 0; i < nF; i++) {
        if (!want_parse[i]) continue;
        if (!(extract ? extract(root, i) : lg_extract_file(root, i))) skipped++;
    }
    free(want_parse);

    /* edges: remap old edges whose SOURCE file was reused (the target is
     * re-resolved by identity — it may live in a re-parsed file) */
    for (int i = 0; i < onE; i++) {
        int os = (int)oE[i].src, od = (int)oE[i].dst;
        if (os >= onN || od >= onN) continue;
        int nsrc = old2new[os];
        if (nsrc < 0) continue;   /* source file gone or re-parsed */
        int of = oN[od].fidx;
        if (of < 0 || of >= onF || ofile_status[of] == 0) continue;
        int nfidx = ofile_newmap[of];
        unsigned b = lg_fnv(oN[od].name) & (LG_HNAME - 1);
        int ndst = -1;
        for (int j = nhash[b]; j >= 0; j = nnext[j])
            if (N[j].fidx == nfidx && N[j].type == oN[od].type &&
                !strcmp(N[j].name, oN[od].name)) { ndst = j; break; }
        if (ndst < 0) continue;   /* target vanished with the re-parse */
        lg_edge_add(nsrc, ndst, oE[i].rel, oE[i].conf);
    }

    /* edges from freshly parsed files */
    lg_resolve_calls();
    lg_resolve_includes();

    int ncomm = lg_communities();

    *observed = (lg_build_observation_t){reused, reparsed, gone, skipped, ncomm};
    return 0;
}

static int lg_cmd_build(char *a[], int n, jash_writer_t w, void *ctx) {
    int json = lg_has(a, n, "--json");
    const char *dir = lg_pos(a, n, 0);
    if (!dir) dir = ".";

    char root[PATH_MAX];   /* realpath writes up to PATH_MAX — was 800 */
    if (!realpath(dir, root)) {
        jash_writef(jash_stderr_writer, 0, "graph: cannot resolve '%s'\n", dir);
        return 1;
    }
    char gp[700];
    if (!lg_graph_path(gp, sizeof gp)) {
        jash_writes(jash_stderr_writer, 0, "graph: HOME not set\n");
        return 1;
    }

    int lk = lg_lock();
    lg_truncated = 0;
    onN = onE = onF = 0;
    lg_load_into(gp, oN, &onN, oE, &onE, oF, &onF);

    lg_build_observation_t observed;
    if (lg_build_inventory(root, NULL, NULL, &observed)) { lg_unlock(lk); return 1; }
    int reused = observed.reused, reparsed = observed.reparsed, gone = observed.gone;
    int skipped = observed.skipped, ncomm = observed.communities;

    /* nothing changed → keep the old file (its mtime IS the build age) */
    if (reparsed > 0 || gone > 0 || onF == 0) lg_store(gp);
    if (lk >= 0) lg_unlock(lk);

    {
        const char *k = jash_repo_key();
        char det[600];
        snprintf(det, sizeof det,
                 "files=%d nodes=%d edges=%d reused=%d reparsed=%d gone=%d skipped=%d repo=%s",
                 nF, nN, nE, reused, reparsed, gone, skipped,
                 (k && k[0] != '-') ? k : "-");
        jash_sigil_attest("graph-build", det);
    }

    if (json) {
        const char *k = jash_repo_key();
        lg_out("{\"files\":%d,\"reused\":%d,\"reparsed\":%d,\"gone\":%d,"
               "\"skipped\":%d,\"nodes\":%d,\"edges\":%d,\"communities\":%d,"
               "\"repo\":\"%s\"}\n",
               nF, reused, reparsed, gone, skipped, nN, nE, ncomm,
               (k && k[0] != '-') ? k : "-");
    } else {
        lg_out("build: %d files (%d reused, %d re-parsed, %d gone, %d skipped)"
               " — %d nodes, %d edges, %d communities%s\n",
               nF, reused, reparsed, gone, skipped, nN, nE, ncomm,
               lg_truncated ? " [TRUNCATED: store caps hit]" : "");
    }
    lg_flush(w, ctx, lg_budget(a, n));
    return 0;
}

/* ── W2: explain ───────────────────────────────────────────────────────────── */

static int lg_cmd_explain(char *a[], int n, jash_writer_t w, void *ctx) {
    int json = lg_has(a, n, "--json");
    const char *q = lg_pos(a, n, 0);
    if (!q) {
        jash_writes(jash_stderr_writer, 0, "usage: graph explain <name> [--budget N] [--json]\n");
        return 2;
    }
    if (!lg_load()) {
        jash_writes(jash_stderr_writer, 0,
            "graph: no graph for this repo — run 'graph build'\n");
        return 1;
    }
    int u = lg_lookup(q);
    if (u < 0) {
        if (json) {
            jash_structured_output_t out;
            lg_flushed = 1;
            if (jash_structured_begin(&out, w, ctx)) return 125;
            jash_jsonw_raw(&out.json, "{\"error\":\"not found\",\"query\":");
            jash_jsonw_string_or_bytes(&out.json, q);
            jash_jsonw_raw(&out.json, "}\n");
            return jash_structured_finish(&out, 1);
        }
        else {
            lg_out("graph: '%s' not found\n", q);
            size_t ql = strlen(q);
            int sug = 0;
            for (int i = 0; i < nN && sug < 3; i++) {
                if (N[i].type == 'M') continue;
                if (!strncmp(N[i].name, q, ql) || strstr(N[i].name, q)) {
                    lg_out("  did you mean: %s (%s %s:%ld)\n",
                           N[i].name, lg_type_name(N[i].type),
                           F[N[i].fidx].path, N[i].line);
                    sug++;
                }
            }
            lg_flush(w, ctx, 0);
        }
        return 1;
    }
    lg_build_adj();
    if (json) {
        char en[300], ef[800];
        jash_json_esc(N[u].name, en, sizeof en);
        jash_json_esc(F[N[u].fidx].path, ef, sizeof ef);
        lg_out("{\"name\":\"%s\",\"type\":\"%c\",\"file\":\"%s\",\"line\":%ld,"
               "\"community\":%d,\"degree\":%d,\"edges\":[",
               en, N[u].type, ef, N[u].line, N[u].community, deg[u]);
        int shown = 0, total = deg[u];
        for (int k = adj_off[u]; k < adj_off[u + 1] && shown < 20; k++, shown++) {
            int ei = adj_edge[k];
            int other = adj_dst[k];
            int out = (int)E[ei].src == u;
            char eo[300];
            jash_json_esc(N[other].name, eo, sizeof eo);
            lg_out("%s{\"dir\":\"%s\",\"rel\":\"%s\",\"target\":\"%s\",\"conf\":\"%s\"}",
                   shown ? "," : "", out ? "out" : "in",
                   lg_rel_out(E[ei].rel), eo, lg_conf_name(E[ei].conf));
        }
        lg_out("],\"truncated\":%s}\n", total > 20 ? "true" : "false");
    } else {
        lg_out("%s  %s  %s:%ld  community %d  degree %d\n",
               N[u].name, lg_type_name(N[u].type), F[N[u].fidx].path,
               N[u].line, N[u].community, deg[u]);
        int shown = 0;
        for (int k = adj_off[u]; k < adj_off[u + 1] && shown < 20; k++, shown++) {
            int ei = adj_edge[k];
            int other = adj_dst[k];
            if ((int)E[ei].src == u)
                lg_out("  --%s--> %s (%s:%ld) [%s]\n",
                       lg_rel_out(E[ei].rel), N[other].name,
                       F[N[other].fidx].path, N[other].line, lg_conf_name(E[ei].conf));
            else
                lg_out("  <--%s-- %s (%s:%ld) [%s]\n",
                       lg_rel_in(E[ei].rel), N[other].name,
                       F[N[other].fidx].path, N[other].line, lg_conf_name(E[ei].conf));
        }
        if (deg[u] > 20)
            lg_out("  … %d more connections (cap 20 — refine with --json)\n", deg[u] - 20);
    }
    lg_flush(w, ctx, lg_budget(a, n));
    return 0;
}

/* ── W2: path (BFS, undirected, direction declared per hop) ────────────────── */

static int lg_cmd_path(char *a[], int n, jash_writer_t w, void *ctx) {
    int json = lg_has(a, n, "--json");
    const char *qa = lg_pos(a, n, 0), *qb = lg_pos(a, n, 1);
    if (!qa || !qb) {
        jash_writes(jash_stderr_writer, 0, "usage: graph path <A> <B> [--json]\n");
        return 2;
    }
    if (!lg_load()) {
        jash_writes(jash_stderr_writer, 0,
            "graph: no graph for this repo — run 'graph build'\n");
        return 1;
    }
    int sa = lg_lookup(qa), sb = lg_lookup(qb);
    if (sa < 0) { jash_writef(jash_stderr_writer, 0, "graph: no node matching '%s'\n", qa); return 1; }
    if (sb < 0) { jash_writef(jash_stderr_writer, 0, "graph: no node matching '%s'\n", qb); return 1; }
    lg_build_adj();

    memset(visited, 0, nN);
    int qh = 0, qt = 0, searched = 0;
    queue[qt++] = sa; visited[sa] = 1; prev_node[sa] = -1; prev_edge[sa] = -1;
    int found = 0;
    while (qh < qt) {
        int u = queue[qh++]; searched++;
        if (u == sb) { found = 1; break; }
        for (int k = adj_off[u]; k < adj_off[u + 1]; k++) {
            int v = adj_dst[k];
            if (visited[v]) continue;
            visited[v] = 1;
            prev_node[v] = u; prev_edge[v] = adj_edge[k];
            queue[qt++] = v;
        }
    }
    if (!found) {
        if (json) lg_out("{\"found\":false,\"searched\":%d}\n", searched);
        else lg_out("no path (%d nodes searched)\n", searched);
        lg_flush(w, ctx, lg_budget(a, n));
        return 1;
    }
    /* walk back sb→sa, then print forward */
    int chain[4096]; int cn = 0;
    for (int u = sb; u >= 0 && cn < 4096; u = prev_node[u]) chain[cn++] = u;
    if (json) {
        lg_out("{\"found\":true,\"searched\":%d,\"hops\":[", searched);
        for (int i = cn - 1; i > 0; i--) {
            int u = chain[i], v = chain[i - 1];
            int ei = 0;
            for (int k = adj_off[u]; k < adj_off[u + 1]; k++)
                if (adj_dst[k] == v) { ei = adj_edge[k]; break; }
            int fwd = (int)E[ei].src == u;
            char eu[300], ev[300];
            jash_json_esc(N[u].name, eu, sizeof eu);
            jash_json_esc(N[v].name, ev, sizeof ev);
            lg_out("%s{\"from\":\"%s\",\"rel\":\"%s\",\"dir\":\"%s\",\"to\":\"%s\"}",
                   i < cn - 1 ? "," : "", eu, lg_rel_out(E[ei].rel),
                   fwd ? "fwd" : "rev", ev);
        }
        lg_out("]}\n");
    } else {
        lg_out("%s", N[chain[cn - 1]].name);
        for (int i = cn - 1; i > 0; i--) {
            int u = chain[i], v = chain[i - 1];
            int ei = 0;
            for (int k = adj_off[u]; k < adj_off[u + 1]; k++)
                if (adj_dst[k] == v) { ei = adj_edge[k]; break; }
            if ((int)E[ei].src == u)
                lg_out(" --%s--> %s", lg_rel_out(E[ei].rel), N[v].name);
            else
                lg_out(" <--%s-- %s", lg_rel_in(E[ei].rel), N[v].name);
        }
        lg_out("\n");
    }
    lg_flush(w, ctx, lg_budget(a, n));
    return 0;
}

/* ── W2: god-nodes ─────────────────────────────────────────────────────────── */

static int lg_cmd_god(char *a[], int n, jash_writer_t w, void *ctx) {
    int json = lg_has(a, n, "--json");
    int top = lg_top(a, n, 10);
    if (!lg_load()) {
        jash_writes(jash_stderr_writer, 0,
            "graph: no graph for this repo — run 'graph build'\n");
        return 1;
    }
    lg_build_adj();
    int *idx = calloc(nN > 0 ? nN : 1, sizeof(int));
    if (!idx) return 1;
    for (int i = 0; i < nN; i++) idx[i] = i;
    /* top-K selection — full sort is wasted work for a top-N listing */
    int lim = top < nN ? top : nN;
    for (int i = 0; i < lim; i++) {
        int m = i;
        for (int j = i + 1; j < nN; j++) if (deg[idx[j]] > deg[idx[m]]) m = j;
        int t = idx[i]; idx[i] = idx[m]; idx[m] = t;
    }
    if (json) {
        lg_out("[");
        int shown = 0;
        for (int i = 0; i < nN && shown < top; i++) {
            int u = idx[i];
            char en[300]; jash_json_esc(N[u].name, en, sizeof en);
            lg_out("%s{\"name\":\"%s\",\"degree\":%d,\"type\":\"%c\",\"file\":\"%s\","
                   "\"line\":%ld,\"community\":%d}",
                   shown ? "," : "", en, deg[u], N[u].type,
                   F[N[u].fidx].path, N[u].line, N[u].community);
            shown++;
        }
        lg_out("]\n");
    } else {
        lg_out("═══ GOD NODES (top %d, by total degree) ═══\n", top);
        int shown = 0;
        for (int i = 0; i < nN && shown < top; i++, shown++) {
            int u = idx[i];
            lg_out("  %3d  %-30s %-6s %s:%ld  comm %d\n",
                   deg[u], N[u].name, lg_type_name(N[u].type),
                   F[N[u].fidx].path, N[u].line, N[u].community);
        }
        if (!shown) lg_out("  (empty graph)\n");
    }
    free(idx);
    lg_flush(w, ctx, lg_budget(a, n));
    return 0;
}

/* ── W2: communities ───────────────────────────────────────────────────────── */

static int lg_cmd_communities(char *a[], int n, jash_writer_t w, void *ctx) {
    int json = lg_has(a, n, "--json");
    int top = lg_top(a, n, 10);
    if (!lg_load()) {
        jash_writes(jash_stderr_writer, 0,
            "graph: no graph for this repo — run 'graph build'\n");
        return 1;
    }
    lg_build_adj();
    int ncomm = 0;
    for (int i = 0; i < nN; i++) if (N[i].community + 1 > ncomm) ncomm = N[i].community + 1;
    int *sizes = calloc(ncomm > 0 ? ncomm : 1, sizeof(int));
    int *order = calloc(ncomm > 0 ? ncomm : 1, sizeof(int));
    int *t3all = calloc((ncomm > 0 ? ncomm : 1) * 3, sizeof(int));
    if (!sizes || !order || !t3all) { free(sizes); free(order); free(t3all); return 1; }
    for (int i = 0; i < ncomm; i++) { order[i] = i; t3all[i * 3] = t3all[i * 3 + 1] = t3all[i * 3 + 2] = -1; }
    /* one pass: size per community + its top-3 nodes by degree */
    for (int u = 0; u < nN; u++) {
        int c = N[u].community;
        sizes[c]++;
        int *t3 = &t3all[c * 3];
        for (int s = 0; s < 3; s++) {
            if (t3[s] < 0 || deg[u] > deg[t3[s]]) {
                for (int r = 2; r > s; r--) t3[r] = t3[r - 1];
                t3[s] = u;
                break;
            }
        }
    }
    /* top-K selection by size */
    int lim = top < ncomm ? top : ncomm;
    for (int i = 0; i < lim; i++) {
        int m = i;
        for (int j = i + 1; j < ncomm; j++) if (sizes[order[j]] > sizes[order[m]]) m = j;
        int t = order[i]; order[i] = order[m]; order[m] = t;
    }
    if (json) lg_out("[");
    else lg_out("═══ COMMUNITIES (top %d of %d, by size) ═══\n", top, ncomm);
    int shown = 0;
    for (int i = 0; i < lim; i++, shown++) {
        int c = order[i];
        int *t3 = &t3all[c * 3];
        if (json) {
            lg_out("%s{\"id\":%d,\"size\":%d,\"top\":[", shown ? "," : "", c, sizes[c]);
            for (int s = 0; s < 3 && t3[s] >= 0; s++) {
                char en[300]; jash_json_esc(N[t3[s]].name, en, sizeof en);
                lg_out("%s{\"name\":\"%s\",\"degree\":%d}", s ? "," : "", en, deg[t3[s]]);
            }
            lg_out("]}");
        } else {
            lg_out("community %d — %d nodes:", c, sizes[c]);
            for (int s = 0; s < 3 && t3[s] >= 0; s++)
                lg_out("%s %s(%d)", s ? "," : "", N[t3[s]].name, deg[t3[s]]);
            lg_out("\n");
        }
    }
    if (json) lg_out("]\n");
    free(sizes); free(order); free(t3all);
    lg_flush(w, ctx, lg_budget(a, n));
    return 0;
}

/* ── W4: hot — pain = touches + 3×fails, joined with the gap queue ─────────── */

static const char *lg_basename(const char *p) {
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

static int lg_cmd_hot(char *a[], int n, jash_writer_t w, void *ctx) {
    int json = lg_has(a, n, "--json");
    int top = lg_top(a, n, 10);
    if (!lg_load()) {
        jash_writes(jash_stderr_writer, 0,
            "graph: no graph for this repo — run 'graph build'\n");
        return 1;
    }
    const char *home = getenv("HOME");
    if (!home || !*home) return 1;
    char cp[700];
    const char *k = jash_repo_key();
    if (k && k[0] != '-')
        snprintf(cp, sizeof cp, "%s/.jash/chain-%s.log", home, k);
    else
        snprintf(cp, sizeof cp, "%s/.jash/chain.log", home);
    FILE *f = fopen(cp, "r");
    if (!f) {
        lg_out("%s", json ? "{\"error\":\"no chain\"}\n"
                          : "graph: no chain yet — nothing to learn from (run commands with JASH_SIGIL=1)\n");
        lg_flush(w, ctx, lg_budget(a, n));
        return 0;
    }
    lg_build_adj();

    /* basename → file index */
    for (int i = 0; i < LG_HFILE; i++) bhash[i] = -1;
    for (int i = 0; i < nF; i++) {
        unsigned b = lg_fnv(lg_basename(F[i].path)) & (LG_HFILE - 1);
        bnext[i] = bhash[b]; bhash[b] = i;
    }

    int *touches = calloc(nF > 0 ? nF : 1, sizeof(int));
    int *fails = calloc(nF > 0 ? nF : 1, sizeof(int));
    if (!touches || !fails) { fclose(f); free(touches); free(fails); return 1; }
    int lines = 0, files_touched = 0, total_fails = 0, stamp = 0;
    char line[2048];
    while (fgets(line, sizeof line, f)) {
        const char *cm = strstr(line, " cmd=");
        if (!cm) continue;
        lines++;
        int rc = 0;
        const char *r = strstr(line, " rc=");
        if (r) rc = atoi(r + 4);
        stamp++;
        /* every path component of every token is a candidate file basename */
        char tok[512]; int tl = 0;
        for (const char *p = cm + 5;; p++) {
            char c = *p;
            if (c && c != ' ' && c != '\t' && c != '\n' && c != '"' && c != '\'') {
                if (tl < 500) tok[tl++] = c;
                continue;
            }
            tok[tl] = 0;
            if (tl > 0) {
                char comp[512];
                snprintf(comp, sizeof comp, "%s", tok);
                char *save = NULL;
                for (char *t = strtok_r(comp, "/", &save); t; t = strtok_r(NULL, "/", &save)) {
                    unsigned b = lg_fnv(t) & (LG_HFILE - 1);
                    for (int i = bhash[b]; i >= 0; i = bnext[i]) {
                        if (strcmp(lg_basename(F[i].path), t)) continue;
                        if (fstamp[i] == stamp) continue;
                        fstamp[i] = stamp;
                        touches[i]++;
                        if (rc != 0) { fails[i]++; total_fails++; }
                    }
                }
            }
            if (!c) break;
            tl = 0;
        }
    }
    fclose(f);
    for (int i = 0; i < nF; i++) if (touches[i] > 0) files_touched++;

    /* gap queue cross-reference: demand the fleet already reported */
    static char gapbuf[512][512]; int ngap = 0;
    {
        char gf[700];
        snprintf(gf, sizeof gf, "%s/.jash/gaps.tsv", home);
        FILE *g = fopen(gf, "r");
        if (g) {
            char gl[1600];
            while (ngap < 512 && fgets(gl, sizeof gl, g)) {
                char *fld[8] = {0};
                int nf2 = lg_split(gl, fld, 8);
                if (nf2 > 5 && fld[5]) snprintf(gapbuf[ngap++], 512, "%s", fld[5]);
            }
            fclose(g);
        }
    }

    /* top node per touched file */
    int *idx = calloc(nF > 0 ? nF : 1, sizeof(int));
    if (!idx) { free(touches); free(fails); return 1; }
    for (int i = 0; i < nF; i++) idx[i] = i;
    /* top-K selection by pain */
    int lim = top < nF ? top : nF;
    for (int i = 0; i < lim; i++) {
        int m = i;
        for (int j = i + 1; j < nF; j++)
            if (touches[idx[j]] + 3 * fails[idx[j]] > touches[idx[m]] + 3 * fails[idx[m]]) m = j;
        int t = idx[i]; idx[i] = idx[m]; idx[m] = t;
    }
    if (json) {
        lg_out("{\"lines\":%d,\"files_touched\":%d,\"failures\":%d,\"hot\":[",
               lines, files_touched, total_fails);
    } else {
        lg_out("═══ HOT FILES (pain = touches + 3×fails, from attested chain) ═══\n");
    }
    int shown = 0;
    for (int i = 0; i < nF && shown < top; i++) {
        int fi = idx[i];
        int pain = touches[fi] + 3 * fails[fi];
        if (pain <= 0) break;
        int topnode = -1;
        for (int u = 0; u < nN; u++)
            if (N[u].fidx == fi && N[u].type != 'M' &&
                (topnode < 0 || deg[u] > deg[topnode])) topnode = u;
        const char *bn = lg_basename(F[fi].path);
        int gap = 0;
        for (int g = 0; g < ngap; g++)
            if (strstr(gapbuf[g], bn) ||
                (topnode >= 0 && strstr(gapbuf[g], N[topnode].name))) { gap = 1; break; }
        if (json) {
            char ef[800], en[300];
            jash_json_esc(F[fi].path, ef, sizeof ef);
            jash_json_esc(topnode >= 0 ? N[topnode].name : "", en, sizeof en);
            lg_out("%s{\"file\":\"%s\",\"pain\":%d,\"touches\":%d,\"fails\":%d,"
                   "\"top_node\":\"%s\",\"gap\":%s}",
                   shown ? "," : "", ef, pain, touches[fi], fails[fi], en,
                   gap ? "true" : "false");
        } else {
            lg_out("  %3d  %-40s (%d touches, %d fail%s)  top: %s%s\n",
                   pain, F[fi].path, touches[fi], fails[fi],
                   fails[fi] == 1 ? "" : "s",
                   topnode >= 0 ? N[topnode].name : "-",
                   gap ? "  [gap]" : "");
        }
        shown++;
    }
    if (json) lg_out("]}\n");
    else {
        if (!shown) lg_out("  (no graph files appear in this repo's chain yet)\n");
        lg_out("%d files touched, %d failures, %d chain lines scanned\n",
               files_touched, total_fails, lines);
    }
    free(touches); free(fails); free(idx);
    lg_flush(w, ctx, lg_budget(a, n));
    return 0;
}

/* ── W2: stats ─────────────────────────────────────────────────────────────── */

static int lg_cmd_stats(char *a[], int n, jash_writer_t w, void *ctx) {
    int json = lg_has(a, n, "--json");
    char gp[700];
    if (!lg_graph_path(gp, sizeof gp) || !lg_load()) {
        jash_writes(jash_stderr_writer, 0,
            "graph: no graph for this repo — run 'graph build'\n");
        return 1;
    }
    int tf = 0, ts = 0, te = 0, tt = 0, tm = 0;
    for (int i = 0; i < nN; i++)
        switch (N[i].type) {
            case 'F': tf++; break; case 'S': ts++; break; case 'E': te++; break;
            case 'T': tt++; break; case 'M': tm++; break;
        }
    int rc = 0, rk = 0, ri = 0, ce = 0, ci = 0;
    for (int i = 0; i < nE; i++) {
        if (E[i].rel == 'C') rc++; else if (E[i].rel == 'K') rk++; else ri++;
        if (E[i].conf == 'E') ce++; else ci++;
    }
    int ncomm = 0;
    for (int i = 0; i < nN; i++) if (N[i].community + 1 > ncomm) ncomm = N[i].community + 1;
    long age = -1;
    struct stat st;
    if (stat(gp, &st) == 0) age = (long)time(NULL) - (long)st.st_mtime;
    if (json) {
        lg_out("{\"nodes\":%d,\"funcs\":%d,\"structs\":%d,\"enums\":%d,"
               "\"typedefs\":%d,\"modules\":%d,\"edges\":%d,\"calls\":%d,"
               "\"contains\":%d,\"includes\":%d,\"extracted\":%d,\"inferred\":%d,"
               "\"files\":%d,\"communities\":%d,\"age_sec\":%ld}\n",
               nN, tf, ts, te, tt, tm, nE, rc, rk, ri, ce, ci, nF, ncomm, age);
    } else {
        lg_out("nodes: %d (func %d, struct %d, enum %d, typedef %d, module %d)\n",
               nN, tf, ts, te, tt, tm);
        lg_out("edges: %d (calls %d, contains %d, includes %d; EXTRACTED %d, INFERRED %d)\n",
               nE, rc, rk, ri, ce, ci);
        lg_out("files: %d\ncommunities: %d\n", nF, ncomm);
        if (age >= 0) {
            if (age < 60) lg_out("built: %lds ago\n", age);
            else if (age < 3600) lg_out("built: %ldm ago\n", age / 60);
            else if (age < 86400) lg_out("built: %ldh ago\n", age / 3600);
            else lg_out("built: %ldd ago\n", age / 86400);
        }
    }
    lg_flush(w, ctx, lg_budget(a, n));
    return 0;
}

/* ── W7: push/sync — the graph joins the mesh fabric ───────────────────────── */

static int lg_cmd_push(char *a[], int n, jash_writer_t w, void *ctx) {
    int json = lg_has(a, n, "--json");
    char gp[700];
    if (!lg_graph_path(gp, sizeof gp) || !lg_load()) {
        jash_writes(jash_stderr_writer, 0,
            "graph: nothing to push — run 'graph build'\n");
        return 1;
    }
    if (!lg_mesh_active()) {
        jash_writes(jash_stderr_writer, 0, "graph: mesh inactive (no ~/.jash/mesh)\n");
        return 1;
    }
    char mp[760];
    if (!lg_mesh_graph_path(mp, sizeof mp)) return 1;

    int lk = lg_mesh_lock();
    onN = onE = onF = 0;
    lg_load_into(mp, oN, &onN, oE, &onE, oF, &onF);
    lg_merge_fleet();           /* local ∪ fleet, in memory */
    int merged = lg_merge_nodes_added + lg_merge_edges_added;
    lg_communities();           /* merged graph gets fresh communities */
    lg_store(mp);               /* publish the union — local TSV untouched */
    if (lk >= 0) lg_unlock(lk);

    const char *agent = getenv("JASH_AGENT_ID");
    {
        char det[600];
        snprintf(det, sizeof det, "nodes=%d edges=%d merged_from_fleet=%d agent=%s",
                 nN, nE, merged, (agent && *agent) ? agent : "-");
        jash_sigil_attest("graph-push", det);
    }
    if (json)
        lg_out("{\"nodes\":%d,\"edges\":%d,\"merged_from_fleet\":%d,\"agent\":\"%s\"}\n",
               nN, nE, merged, (agent && *agent) ? agent : "-");
    else
        lg_out("push: %d nodes, %d edges → mesh (+%d merged from fleet)\n",
               nN, nE, merged);
    lg_flush(w, ctx, lg_budget(a, n));
    return 0;
}

static int lg_cmd_sync(char *a[], int n, jash_writer_t w, void *ctx) {
    int json = lg_has(a, n, "--json");
    if (!lg_mesh_active()) {
        jash_writes(jash_stderr_writer, 0, "graph: mesh inactive (no ~/.jash/mesh)\n");
        return 1;
    }
    char mp[760];
    if (!lg_mesh_graph_path(mp, sizeof mp)) return 1;

    /* read the fleet graph under the mesh lock, release, then merge locally */
    int mlk = lg_mesh_lock();
    onN = onE = onF = 0;
    int has_fleet = lg_load_into(mp, oN, &onN, oE, &onE, oF, &onF);
    if (mlk >= 0) lg_unlock(mlk);
    if (!has_fleet) {
        jash_writes(jash_stderr_writer, 0,
            "graph: no fleet graph yet — a teammate must 'graph push' first\n");
        return 1;
    }

    char gp[700];
    if (!lg_graph_path(gp, sizeof gp)) return 1;
    int lk = lg_lock();
    int had_local = lg_load();   /* empty local is fine: sync adopts the fleet */
    (void)had_local;
    lg_merge_fleet();            /* local ∪ fleet, in memory */
    int dn = lg_merge_nodes_added, de = lg_merge_edges_added, up = lg_merge_upgraded;
    if (dn > 0 || de > 0 || up > 0 || !had_local) {
        lg_communities();
        lg_store(gp);
    }
    if (lk >= 0) lg_unlock(lk);

    {
        char det[600];
        snprintf(det, sizeof det, "nodes=+%d edges=+%d upgraded=%d total_nodes=%d total_edges=%d",
                 dn, de, up, nN, nE);
        jash_sigil_attest("graph-sync", det);
    }
    if (json)
        lg_out("{\"nodes_added\":%d,\"edges_added\":%d,\"upgraded\":%d,\"nodes\":%d,\"edges\":%d}\n",
               dn, de, up, nN, nE);
    else
        lg_out("sync: +%d nodes, +%d edges%s from fleet (union, deterministic)\n", dn, de,
               up > 0 ? " (+confidence upgrades)" : "");
    lg_flush(w, ctx, lg_budget(a, n));
    return 0;
}

/* ── dispatch ──────────────────────────────────────────────────────────────── */

static int lg_dispatch_impl(jash_state_t *s, jash_writer_t w, void *ctx,
                       char *tok[], int nt) {
    if (nt == 0) {
        jash_writes(w, ctx,
            "graph — living graph: the repo knowledge map that learns from use (ADR-0053)\n"
            "  graph build [dir] [--json]          walk .c/.h, incremental re-parse, persist graph\n"
            "  graph explain <name> [--budget N] [--json]   connections with declared confidence\n"
            "  graph path <A> <B> [--json]         BFS hop chain, direction per hop\n"
            "  graph god-nodes [--top N] [--json]  highest-degree nodes\n"
            "  graph communities [--top N] [--json]   label-propagation clusters\n"
            "  graph hot [--top N] [--json]        pain = touches + 3×fails, from attested chain\n"
            "  graph stats [--json]                counts, confidence split, build age\n"
            "  graph push [--json]                 publish local graph to the mesh (union merge)\n"
            "  graph sync [--json]                 pull the fleet graph (union merge, delta declared)\n"
            "  graph persists in ~/.jash/graph-<repokey>.tsv — every verb reads it, only build writes\n");
        return 0;
    }
    if (!lg_alloc()) {
        jash_writes(jash_stderr_writer, 0, "graph: out of memory\n");
        return 1;
    }
    const char *sub = tok[0];
    char **a = tok + 1; int n = nt - 1;
    if (!strcmp(sub, "build"))       return lg_cmd_build(a, n, w, ctx);
    if (!strcmp(sub, "explain"))     return lg_cmd_explain(a, n, w, ctx);
    if (!strcmp(sub, "path"))        return lg_cmd_path(a, n, w, ctx);
    if (!strcmp(sub, "god-nodes"))   return lg_cmd_god(a, n, w, ctx);
    if (!strcmp(sub, "communities")) return lg_cmd_communities(a, n, w, ctx);
    if (!strcmp(sub, "hot"))         return lg_cmd_hot(a, n, w, ctx);
    if (!strcmp(sub, "stats"))       return lg_cmd_stats(a, n, w, ctx);
    if (!strcmp(sub, "push"))        return lg_cmd_push(a, n, w, ctx);
    if (!strcmp(sub, "sync"))        return lg_cmd_sync(a, n, w, ctx);
    (void)s;
    jash_writef(jash_stderr_writer, 0, "graph: unknown subcommand '%s'\n", sub);
    return 2;
}

static int lg_dispatch(jash_state_t *s, jash_writer_t w, void *ctx,
                              char *tok[], int nt) {
    lg_olen = 0; lg_required = 0; lg_obuf[0] = 0;
    lg_output_status = 0; lg_flushed = 0;
    lg_json_mode = lg_has(tok, nt, "--json");
    if (lg_json_mode) jash_budget_unwrap(&w, &ctx);
    int rc = lg_dispatch_impl(s, w, ctx, tok, nt);
    if (lg_output_status) return lg_output_status;
    if (lg_json_mode && !lg_flushed)
        return jash_structured_error(w, ctx, "graph",
                                      rc == 2 ? "invalid_arguments" : "operation_failed",
                                      rc ? rc : 1);
    return rc;
}

int jash_b_graph(jash_state_t *s, jash_writer_t w, void *ctx, const char *args) {
    char buf[4096];
    snprintf(buf, sizeof buf, "%s", args ? args : "");
    char *tok[64]; int nt = 0;
    char *p = buf;
    while (*p && nt < 64) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        char q = 0; char *start;
        if (*p == '"' || *p == '\'') { q = *p; p++; start = p; while (*p && *p != q) p++; }
        else { start = p; while (*p && *p != ' ' && *p != '\t') p++; }
        if (*p) { *p = 0; p++; }
        tok[nt++] = start;
    }
    return lg_dispatch(s, w, ctx, tok, nt);
}

#include "graph_native.inc"

/* ADR-0036 W2: argv entry — quoted names arrive as ONE argv[i] */
int jash_b_graph_argv(jash_state_t *s, jash_writer_t w, void *ctx,
                      int argc, char **argv) {
    int mode = semantic_dispatch_argv(s, w, ctx, "graph", "jash.graph.v1", argc, argv, gn_run);
    if (mode >= 0) return mode;
    return lg_dispatch(s, w, ctx, argv, argc);
}
