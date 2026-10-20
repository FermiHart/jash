/* SPDX-License-Identifier: LicenseRef-SPL-1.0 */
// jash module: sigil — ADR-0003 S1-S9
// Verifiable execution chain: BLAKE3(prev_hash || ts_ns || exit_code || cmd)
// Forward-secure ratchet MACs: tag_n = BLAKE3_keyed(k_n, h_n); k_{n+1} = BLAKE3(k_n || "r")
//
// Chain file: ~/.jash/chain-<repo>.log (fallback chain.log outside a repository)
// Format per line (S7+):
//   <seq> <ts_sec>.<ts_ns> <blake3_hex_64> tag=<tag_hex_64> rc=<rc> cmd=<cmd>
//
// On first append: a strictly recognized SHA-256 chain is archived as .v1.
// On exit: flush in-memory tail and write a closing sentinel.

#include <module/modjash.h>
#include <module/sigil_native.h>
#include <core/argv.h>
#include <core/repo_identity.h>
#include <module/loader.h>
#include <module/sigil.h>
#include <module/jash_crypto.h>
#include <journal/journal.h>  /* JOURNAL_TAG_MAX: core fallback tag wire size */
#include <ed25519.h>          /* W4b: sovereign signatures; symbols from host */
#include <jef_verify.h>       /* W3b: delegate proof verification to libjef     */
#include <theme/io.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/file.h>         /* flock — swarm-safe append */
#include <fcntl.h>            /* O_CREAT|O_EXCL — one-shot checkpoint key */
#include <sys/wait.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>

/* ── Chain state ────────────────────────────────────────────────────── */

static uint8_t  g_prev_hash[32];                      /* BLAKE3 chain tip    */
static uint8_t  g_ratchet_key[32];                    /* forward-secure tag key */
static uint64_t g_seq = 0;                            /* monotonic entry seq */
/* JINV-031: quantas vezes o ratchet avancou. O ratchet avanca exatamente uma
 * vez por entrada, entao esta contagem e uma TESTEMUNHA INDEPENDENTE do
 * tamanho da cadeia — e, por ser forward-secure, nao pode ser rebobinada. */
static uint64_t g_ratchet_advances = 0;
/* S3: nonce da ultima fronteira de segmento, guardado JUNTO da chave (0600).
 * A fronteira mora no `cmd` (unico campo coberto pelo hash), e `cmd` e texto
 * escolhido pelo usuario — sem este nonce qualquer um DIGITARIA uma fronteira.
 * Vazio = nenhuma fronteira; formato de arquivo antigo le como vazio. */
static char     g_ratchet_nonce[33] = {0};
static FILE*    g_chain_fp = NULL;                    /* open log file       */
static uint64_t g_ts_start_ns = 0;                   /* session start time  */
static const char* g_append_block_reason = NULL;
static uint64_t g_active_entries = 0;
static uint64_t g_active_fragments = 0;
static long long g_first_fragment_offset = -1;
static uint64_t g_entries_after_fragment = 0;
/* JINV-103: the full scan tally, so a verdict can be audited instead of
 * trusted. Without these the user is told "malformed" with no way to see
 * what the scanner actually counted. */
static uint64_t g_active_unattested = 0;
static uint64_t g_active_malformed = 0;
static uint64_t g_active_legacy = 0;
static int g_append_block_warned = 0;
static int g_invalid_v1_artifact = 0;
static uint64_t g_v1_entries = 0;
static uint64_t g_v1_fragments = 0;
static int g_pending_v1_migration = 0;
/* A read-only verb suppresses only ITS post_cmd append.  It must not make
 * the rest of an interactive/session script invisible to the ledger. */
static int g_read_only_pending = 0;
static int g_last_command_read_only = 0;
static int g_native_read_only_argc = -1;
static uint8_t g_native_read_only_hash[32];

/* One module instance owns one ledger identity. HOME is mutable shell state,
 * but an open append stream is not: deriving its ratchet/checkpoint paths from
 * a later HOME splits one transaction across two ledgers. Pin every path and
 * the repo scope together before the first scan/open. */
typedef struct {
    int pinned;
    int has_home;
    char home[PATH_MAX];
    char repo_key[18];
    char jash_dir[PATH_MAX];
    char chain[PATH_MAX];
    char ratchet[PATH_MAX];
    char legacy_ratchet[PATH_MAX];
    char checkpoint[PATH_MAX];
    char checkpoint_key[PATH_MAX];
    char transaction_lock[PATH_MAX];
    char quarantine_dir[PATH_MAX];
    char keys_dir[PATH_MAX];
    char ed_seed[PATH_MAX];
    char proof[PATH_MAX];
    char proof_dsse[PATH_MAX];
    char trusted_pub[PATH_MAX];
} sigil_ledger_identity_t;

static sigil_ledger_identity_t g_ledger;

/* W4b: sovereign Ed25519 signing identity (seed at ~/.jash/keys/ed25519.seed) */
static uint8_t  g_ed_pk[ED25519_PUBKEY];
static uint8_t  g_ed_sk[ED25519_SECKEY];
static int      g_ed_ready = 0;

/* ── Wire encoding for the cmd field (JINV-099) ─────────────────────────
 *
 * The chain is a LINE-ORIENTED ledger, but cmd used to be written raw. A
 * command containing a newline therefore wrote SEVERAL physical lines, and
 * the verifier — which reads one line per entry — hashed only the first
 * fragment. Result: `entries_forged` reported for a writer bug, not an
 * attack. (Measured: 797 physical lines / 785 entries / 12 fragments.)
 *
 * Fix: cmd is escaped before it is BOTH hashed and written, making the
 * invariant exact and self-checking:
 *
 *     the chain hash covers exactly the bytes of the cmd field on the line
 *
 * That invariant is backward compatible. For a newline-free, backslash-free
 * command the escape is a no-op, so every already-valid entry — written raw
 * and hashed raw — still verifies bit-for-bit under the new rule. Only
 * entries that were already corrupt stay invalid, and they are classified
 * as corrupted_newline_fragment rather than forged: a known bug must never
 * masquerade as real tampering. Historical bytes are never rewritten. */

/* Hash payload cap. The writer and every verifier MUST derive their cmd
 * bound from this one macro — they disagreed by one byte (JINV-100). */
#define SIGIL_PAYLOAD_MAX          4096
#define SIGIL_PAYLOAD_CMD_MAX(pl)  (SIGIL_PAYLOAD_MAX - (pl) - 1)
/* Worst case escaping doubles the input, plus NUL. */
#define SIGIL_CMD_ESC_MAX          (2 * SIGIL_PAYLOAD_MAX + 1)
/* A read buffer MUST be able to hold the longest line the writer can emit
 * (escaped cmd + context fields), or fgets silently splits one entry into
 * several reads and the extra pieces read as corruption. */
#define SIGIL_LINE_MAX             (SIGIL_CMD_ESC_MAX + 1024)

extern void jash_json_esc(const char* s, char* out, size_t cap);

static void sigil_wire_escape(const char* in, char* out, size_t cap)
{
    size_t o = 0;
    if (!cap) return;
    for (const char* p = in ? in : ""; *p && o + 2 < cap; p++) {
        switch (*p) {
            case '\\': out[o++] = '\\'; out[o++] = '\\'; break;
            case '\n': out[o++] = '\\'; out[o++] = 'n';  break;
            case '\r': out[o++] = '\\'; out[o++] = 'r';  break;
            default:   out[o++] = *p;                    break;
        }
    }
    out[o] = '\0';
}

/* Inverse of sigil_wire_escape — for DISPLAY only. Verification must never
 * unescape: it hashes the field bytes exactly as stored, which is what makes
 * the invariant above hold for both legacy and new entries. */
static void sigil_wire_unescape(const char* in, char* out, size_t cap)
{
    size_t o = 0;
    if (!cap) return;
    for (const char* p = in ? in : ""; *p && o + 1 < cap; p++) {
        if (*p == '\\' && p[1]) {
            switch (*++p) {
                case 'n':  out[o++] = '\n'; continue;
                case 'r':  out[o++] = '\r'; continue;
                case '\\': out[o++] = '\\'; continue;
                default:   out[o++] = '\\'; out[o++] = *p; continue;
            }
        }
        out[o++] = *p;
    }
    out[o] = '\0';
}

/* A physical line is a CONTINUATION of a multi-line legacy entry (the
 * JINV-099 corruption) rather than an entry of its own. Used to classify
 * honestly instead of blaming an attacker. */
static int sigil_line_is_fragment(const char* line)
{
    unsigned long long seq = 0; int n = 0;
    if (sscanf(line, "%llu %n", &seq, &n) == 1 && n > 0 && strstr(line, " tag="))
        return 0;                       /* well-formed entry head */
    return 1;
}

static int sigil_hex64(const char* s)
{
    if (!s || strlen(s) != 64) return 0;
    for (int i = 0; i < 64; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F'))) return 0;
    }
    return 1;
}

static int sigil_fallback_timestamp(const char* ts)
{
    const char* dot = strchr(ts, '.');
    if (!dot || dot == ts || strlen(dot + 1) != 9) return 0;
    for (const char* p = ts; p < dot; p++)
        if (*p < '0' || *p > '9') return 0;
    for (const char* p = dot + 1; *p; p++)
        if (*p < '0' || *p > '9') return 0;
    return 1;
}

/* journal_current_tag() can only expose these lifecycle forms. In particular,
 * an S7 ratchet tag is 64 hex bytes and can never identify a fallback line.
 * `resum` is the current JOURNAL_TAG_MAX-truncated spelling of `resume`. */
static int sigil_fallback_lifecycle_tag(const char* tag, size_t len)
{
    if ((len == 1 && tag[0] == '-') ||
        (len == 5 && memcmp(tag, "start", 5) == 0) ||
        (len == 5 && memcmp(tag, "resum", 5) == 0) ||
        (len == 6 && memcmp(tag, "resume", 6) == 0) ||
        (len == 5 && memcmp(tag, "retry", 5) == 0))
        return 1;
    if (len == 5 && memcmp(tag, "err:", 4) == 0 &&
        tag[4] >= '2' && tag[4] <= '9')
        return 1;
    if (len < 4 || len >= JOURNAL_TAG_MAX || memcmp(tag, "sw:", 3) != 0)
        return 0;
    for (size_t i = 3; i < len; i++) {
        char c = tag[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '-'))
            return 0;
    }
    return 1;
}

/* Core level-LOG records are ledger records, but not cryptographic chain
 * entries. Match the core writer's complete stable wire shape; zero hash plus
 * generic tag/rc/cmd presence is insufficient because that would launder a
 * damaged S7 entry whose 64-hex ratchet tag remains intact. */
static int sigil_line_is_unattested_log(const char* line)
{
    uint64_t seq = 0;
    char ts[32] = {0}, hash[65] = {0};
    int after_hash = 0;
    if (sscanf(line, "%llu %31s %64s%n",
               (unsigned long long*)&seq, ts, hash, &after_hash) < 3)
        return 0;
    if (!sigil_fallback_timestamp(ts) || !sigil_hex64(hash) ||
        strspn(hash, "0") != 64 || after_hash <= 0)
        return 0;

    const char* p = line + after_hash;
    if (strncmp(p, " tag=", 5) != 0) return 0;
    const char* tag = p + 5;
    const char* tag_end = strchr(tag, ' ');
    if (!tag_end || !sigil_fallback_lifecycle_tag(tag, (size_t)(tag_end - tag)))
        return 0;

    p = tag_end;
    if (strncmp(p, " rc=", 4) != 0) return 0;
    char* end = NULL;
    (void)strtol(p + 4, &end, 10);
    if (end == p + 4) return 0;
    /* Pre-context wire shape (older core level-LOG): ` rc=N cmd=...` with
     * no cwd=/repo=/dur=/sess=. Still a journal record — zero hash plus a
     * lifecycle tag that can never be a 64-hex ratchet tag — so it is
     * unattested, never malformed. Without this, ledgers written by older
     * binaries fail-close every agent on the box (JINV-102 class). */
    if (strncmp(end, " cmd=", 5) == 0) return 1;
    if (strncmp(end, " cwd=", 5) != 0) return 0;

    const char* cwd = end + 5;
    const char* repo = strstr(cwd, " repo=");
    if (!repo || memchr(cwd, ' ', (size_t)(repo - cwd))) return 0;
    const char* repo_value = repo + 6;
    const char* dur = strstr(repo_value, " dur=");
    if (!dur || dur == repo_value ||
        memchr(repo_value, ' ', (size_t)(dur - repo_value))) return 0;

    long duration = strtol(dur + 5, &end, 10);
    if (end == dur + 5 || duration < 0 || strncmp(end, " sess=", 6) != 0)
        return 0;
    const char* sess = end + 6;
    const char* cmd = strstr(sess, " cmd=");
    if (!cmd || cmd == sess || memchr(sess, ' ', (size_t)(cmd - sess)))
        return 0;
    return 1;
}

/* ── S3: segment boundary record (rekey/adopt) ──────────────────────────
 *
 * The BLAKE3 payload is ts||rc||cmd — the context fields (cwd=, repo=, ...)
 * are display-only. So the boundary claim MUST live inside `cmd` or a
 * post-hoc editor could add or strip it without breaking a single hash.
 *
 * Because `cmd` is attacker-chosen text, anyone can TYPE this marker. Two
 * things make that harmless:
 *
 *   1. `nonce=` is generated at rekey time and stored beside the new key
 *      (mode 0600). A user typing the marker cannot know it, so a typed
 *      line is not accepted as a boundary.
 *   2. THE RULE: a boundary may only ever make the verdict MORE
 *      conservative, never less. Entries before it are downgraded to
 *      "hash-chained, no position witness" — they are never laundered into
 *      "verified". And the strict `entries_after_boundary == advances`
 *      check still runs, so a late forged boundary cannot mask a
 *      truncation (advances would still exceed the entries after it).
 *
 * Deleting the log AND the key together remains indistinguishable from a
 * fresh install — the limit already declared at cmd_chain_verify_range and
 * answerable only by an external witness (anchoring). */
#define SIGIL_BOUNDARY_MARK "#sigil-boundary v1 "

typedef struct {
    uint64_t physical_lines;
    uint64_t s7_entries;
    uint64_t legacy_entries;
    uint64_t fragments;
    uint64_t malformed;
    long long first_fragment_offset;
    uint64_t entries_after_fragment;
    /* S3: segment accounting. `entries_after_boundary` resets at every
     * boundary, so it counts only what the CURRENT ratchet should witness. */
    uint64_t boundaries;
    uint64_t entries_after_boundary;
    uint64_t bnd_prev_advances;
    uint64_t bnd_prev_entries;
    char     bnd_nonce[33];
    char     bnd_mode[16];
    /* JINV-103: level-LOG records written by c/module/sigil.c — zero hash
     * (LOG computes none) and a free-text journal tag. Structurally valid,
     * simply NOT attested. Counted apart so they never masquerade as S7 and
     * never brick the chain by being mistaken for corruption. */
    uint64_t unattested;
    uint64_t tag_lines;
    /* JINV-122: seq of the last line that parsed far enough to have one. It
     * is fold state like the rest, and it is what makes a resumed scan
     * self-checking: the first line read after a seek must continue it. */
    uint64_t last_seq;
    char last_s7[SIGIL_LINE_MAX];
} sigil_chain_scan_t;

/* JINV-122: the fold, parameterised by where it starts and reporting where it
 * stopped. Every branch below reads and writes only `scan` and the current
 * line — there is no other carried state — which is precisely why the scan
 * can legitimately be resumed from a byte offset. `start` MUST be a line
 * boundary; the only caller that passes non-zero proves that first. */
static int sigil_scan_chain_from(const char* path, sigil_chain_scan_t* scan,
                                 long long start, long long* end_out)
{
    FILE* f = fopen(path, "r");
    if (!f) return -1;
    if (start > 0 && fseeko(f, (off_t)start, SEEK_SET) != 0) { fclose(f); return -1; }

    char line[SIGIL_LINE_MAX];
    for (;;) {
        off_t line_offset = ftello(f);
        if (!fgets(line, sizeof line, f)) break;
        scan->physical_lines++;
        if (!strchr(line, '\n')) {
            if (!feof(f)) {
                int ch;
                while ((ch = fgetc(f)) != '\n' && ch != EOF) {}
                scan->malformed++;
            } else {
                if (scan->first_fragment_offset < 0)
                    scan->first_fragment_offset = (long long)line_offset;
                scan->fragments++;
            }
            continue;
        }

        uint64_t seq = 0;
        char ts[32] = {0}, hash[65] = {0};
        int after_hash = 0;
        int fields = sscanf(line, "%llu %31s %64s%n",
                            (unsigned long long*)&seq, ts, hash, &after_hash);
        if (strstr(line, " tag=")) scan->tag_lines++;
        if (fields < 3) {
            if (scan->first_fragment_offset < 0)
                scan->first_fragment_offset = (long long)line_offset;
            scan->fragments++;
            continue;
        }
        if (scan->first_fragment_offset >= 0)
            scan->entries_after_fragment++;
        scan->last_seq = seq;

        const char* tag = strstr(line, " tag=");
        const char* rc = strstr(line, " rc=");
        const char* cmd = strstr(line, " cmd=");
        int hash_token_ok = sigil_hex64(hash) && after_hash > 0 &&
                            (line[after_hash] == ' ' || line[after_hash] == '\t' ||
                             line[after_hash] == '\r' || line[after_hash] == '\n' ||
                             line[after_hash] == '\0');
        int rc_value = 0;
        int rc_ok = rc && sscanf(rc, " rc=%d", &rc_value) == 1;

        /* JINV-103: level-LOG record — hash field is all zeros because level
         * LOG computes no hash, and `tag=` carries the free-text journal tag
         * (`tag=start`), not a 64-hex authentication tag. Structurally valid,
         * merely unattested. Tested BEFORE the S7 branch only after its exact
         * core wire shape and lifecycle tag have been established. A zeroed
         * S7 hash retains a 64-hex tag and must fall through as damaged S7. */
        /* JINV-108: the tag field is REQUIRED here. The only writer that emits
         * a zero hash (c/module/sigil.c, level-LOG) always emits `tag=` too, so
         * a zero-hash line WITHOUT a tag is not a level-LOG record produced by
         * any jash version. Without this clause such a line was absorbed as
         * "unattested", the genuine-v1 legacy shape (hash + rc + cmd, no tag)
         * below became unreachable for zero-hash chains, and a real v1 ledger
         * was bricked with `missing_scoped_ratchet` instead of migrating. */
        if (sigil_line_is_unattested_log(line)) {
            scan->unattested++;
            continue;
        }

        if (hash_token_ok && tag && rc_ok && cmd) {
            char taghex[65] = {0};
            int after_tag = 0;
            if (sscanf(tag, " tag=%64s%n", taghex, &after_tag) == 1 &&
                sigil_hex64(taghex) &&
                (tag[after_tag] == ' ' || tag[after_tag] == '\t' ||
                 tag[after_tag] == '\r' || tag[after_tag] == '\n' ||
                 tag[after_tag] == '\0')) {
                scan->s7_entries++;
                snprintf(scan->last_s7, sizeof scan->last_s7, "%s", line);
                /* S3: a boundary belongs to the segment it CLOSES — it is
                 * sealed with the outgoing key, so it is counted before the
                 * reset, never against the incoming ratchet. */
                if (strncmp(cmd + 5, SIGIL_BOUNDARY_MARK,
                            strlen(SIGIL_BOUNDARY_MARK)) == 0) {
                    unsigned long long pa = 0, pe = 0;
                    char nonce[33] = {0}, mode[16] = {0};
                    const char* p;
                    if ((p = strstr(cmd, " mode=")))
                        sscanf(p, " mode=%15s", mode);
                    if ((p = strstr(cmd, " prev_advances=")))
                        sscanf(p, " prev_advances=%llu", &pa);
                    if ((p = strstr(cmd, " prev_entries=")))
                        sscanf(p, " prev_entries=%llu", &pe);
                    if ((p = strstr(cmd, " nonce=")))
                        sscanf(p, " nonce=%32s", nonce);
                    scan->boundaries++;
                    scan->entries_after_boundary = 0;
                    scan->bnd_prev_advances = (uint64_t)pa;
                    scan->bnd_prev_entries  = (uint64_t)pe;
                    snprintf(scan->bnd_nonce, sizeof scan->bnd_nonce, "%s", nonce);
                    snprintf(scan->bnd_mode,  sizeof scan->bnd_mode,  "%s", mode);
                } else {
                    scan->entries_after_boundary++;
                }
                continue;
            }
        } else if (hash_token_ok && !tag && rc_ok && cmd) {
            scan->legacy_entries++;
            continue;
        }
        scan->malformed++;
    }
    /* Where the fold stopped is the only offset a checkpoint may ever claim,
     * and it is read after the loop so a short read or an error leaves it
     * pointing at bytes that really were folded. */
    if (end_out) {
        off_t at = ftello(f);
        *end_out = (at < 0) ? -1 : (long long)at;
    }
    int ok = ferror(f) ? -1 : 0;
    if (fclose(f) != 0) ok = -1;
    return ok;
}

static int sigil_scan_chain(const char* path, sigil_chain_scan_t* scan)
{
    memset(scan, 0, sizeof *scan);
    scan->first_fragment_offset = -1;
    return sigil_scan_chain_from(path, scan, 0, NULL);
}

/* ── Helpers ────────────────────────────────────────────────────────── */

/* Lowercase hex encode. out must hold 2*n+1 bytes. */
static void hexenc(const uint8_t* in, unsigned n, char* out)
{
    static const char* H = "0123456789abcdef";
    for (unsigned i = 0; i < n; i++) { out[i*2]=H[in[i]>>4]; out[i*2+1]=H[in[i]&15]; }
    out[n*2] = '\0';
}
/* Decode exactly n bytes from hex. Returns 0 on success, -1 on bad input. */
static int hexdec(const char* in, uint8_t* out, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        int hi, lo; char a = in[i*2], b = in[i*2+1];
        if      (a>='0'&&a<='9') hi=a-'0'; else if (a>='a'&&a<='f') hi=a-'a'+10;
        else if (a>='A'&&a<='F') hi=a-'A'+10; else return -1;
        if      (b>='0'&&b<='9') lo=b-'0'; else if (b>='a'&&b<='f') lo=b-'a'+10;
        else if (b>='A'&&b<='F') lo=b-'A'+10; else return -1;
        out[i] = (uint8_t)((hi<<4)|lo);
    }
    return 0;
}

static int sigil_path_suffix(char* out, size_t cap, const char* path,
                             const char* suffix);

/* ── base64 (standard alphabet, padded) for the DSSE envelope ─────────── */
static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
/* Encode n bytes → out (must hold 4*((n+2)/3)+1). Returns out length. */
static size_t b64enc(const uint8_t* in, size_t n, char* out)
{
    size_t o = 0, i = 0;
    while (i + 3 <= n) {
        uint32_t v = (in[i]<<16)|(in[i+1]<<8)|in[i+2];
        out[o++]=B64[(v>>18)&63]; out[o++]=B64[(v>>12)&63];
        out[o++]=B64[(v>>6)&63];  out[o++]=B64[v&63]; i+=3;
    }
    if (n - i == 1) {
        uint32_t v = in[i]<<16;
        out[o++]=B64[(v>>18)&63]; out[o++]=B64[(v>>12)&63]; out[o++]='='; out[o++]='=';
    } else if (n - i == 2) {
        uint32_t v = (in[i]<<16)|(in[i+1]<<8);
        out[o++]=B64[(v>>18)&63]; out[o++]=B64[(v>>12)&63]; out[o++]=B64[(v>>6)&63]; out[o++]='=';
    }
    out[o] = '\0';
    return o;
}
static int b64val(char c)
{
    if (c>='A'&&c<='Z') return c-'A';
    if (c>='a'&&c<='z') return c-'a'+26;
    if (c>='0'&&c<='9') return c-'0'+52;
    if (c=='+') return 62;
    if (c=='/') return 63;
    return -1;
}
/* Decode base64 → out (cap bytes). Returns decoded length, or -1 on error. */
static int b64dec(const char* in, uint8_t* out, size_t cap)
{
    size_t o = 0; int q[4], qn = 0;
    for (const char* p = in; *p; p++) {
        if (*p=='\n'||*p=='\r'||*p==' '||*p=='\t') continue;
        if (*p=='=') break;
        int v = b64val(*p); if (v < 0) return -1;
        q[qn++] = v;
        if (qn == 4) {
            if (o+3 > cap) return -1;
            out[o++]=(uint8_t)((q[0]<<2)|(q[1]>>4));
            out[o++]=(uint8_t)((q[1]<<4)|(q[2]>>2));
            out[o++]=(uint8_t)((q[2]<<6)|q[3]); qn=0;
        }
    }
    if (qn == 2) { if (o+1>cap) return -1; out[o++]=(uint8_t)((q[0]<<2)|(q[1]>>4)); }
    else if (qn == 3) {
        if (o+2>cap) return -1;
        out[o++]=(uint8_t)((q[0]<<2)|(q[1]>>4));
        out[o++]=(uint8_t)((q[1]<<4)|(q[2]>>2));
    }
    return (int)o;
}

/* DSSE Pre-Authentication Encoding (what actually gets signed):
 *   "DSSEv1" SP len(type) SP type SP len(body) SP body   (SP = 0x20). */
#define DSSE_PAYLOAD_TYPE "application/vnd.jash.jashproof.v1+text"
#define SIGIL_EXPORT_PAYLOAD_TYPE "application/vnd.jash.sigil.export.v1+json"
#define BEARCODE_MANIFEST_MEDIA_TYPE "application/vnd.bearcode.network-proof-manifest.v1+json"
static int dsse_pae(const char* type, const char* body, size_t body_len,
                    uint8_t* out, size_t cap)
{
    int n = snprintf((char*)out, cap, "DSSEv1 %zu %s %zu ",
                     strlen(type), type, body_len);
    if (n < 0 || (size_t)n + body_len > cap) return -1;
    memcpy(out + n, body, body_len);
    return n + (int)body_len;
}

static const char* sigil_identity_home(void);
static const char* sigil_identity_jash_dir(void);
static const char* sigil_identity_keys_dir(void);
static const char* sigil_identity_ed_seed(void);
static const char* sigil_identity_proof(void);
static const char* sigil_identity_proof_dsse(void);
static const char* sigil_identity_trusted_pub(void);

/* Load (or create) the sovereign Ed25519 seed and derive the keypair. The seed
 * is the device's persistent signing identity — the same key signs every
 * session, so anyone with the public key can verify proofs without a secret. */
static int ed_key_load(void)
{
    if (g_ed_ready) return 0;
    const char* base = sigil_identity_jash_dir();
    const char* dir = sigil_identity_keys_dir();
    const char* path = sigil_identity_ed_seed();

    uint8_t seed[ED25519_SEED];
    struct stat st;
    FILE* f = fopen(path, "rb");
    if (f && fread(seed, 1, ED25519_SEED, f) == ED25519_SEED) {
        fclose(f);
        if (stat(path, &st) == 0 && (st.st_mode & 077) != 0) {
            /* Refuse to use a seed that was ever world-readable. The caller
             * prints the user-facing diagnostic. */
            return -1;
        }
    } else {
        if (f) fclose(f);
        FILE* u = fopen("/dev/urandom", "rb");
        if (!u || fread(seed, 1, ED25519_SEED, u) != ED25519_SEED) { if (u) fclose(u); return -1; }
        fclose(u);
        if (mkdir(base, 0700) != 0 && errno != EEXIST) return -1;
        if (mkdir(dir, 0700) != 0 && errno != EEXIST) return -1;
        int fd = open(path, O_WRONLY|O_CREAT|O_EXCL, 0600);
        if (fd < 0) return -1;
        FILE* wf = fdopen(fd, "wb");
        if (!wf) { close(fd); unlink(path); return -1; }
        if (fwrite(seed, 1, ED25519_SEED, wf) != ED25519_SEED) { fclose(wf); unlink(path); return -1; }
        if (fclose(wf) != 0) { unlink(path); return -1; }
    }
    ed25519_keypair_from_seed(seed, g_ed_pk, g_ed_sk);
    memset(seed, 0, sizeof seed);
    g_ed_ready = 1;
    return 0;
}

static int dsse_envelope_alloc(const char* payload_type,
                               const uint8_t* payload, size_t payload_len,
                               uint8_t** out, size_t* out_len)
{
    *out = NULL;
    *out_len = 0;
    if (ed_key_load() != 0) return -1;

    uint8_t pae[ED25519_MAX_MSG];
    int paelen = dsse_pae(payload_type, (const char*)payload, payload_len,
                          pae, sizeof pae);
    if (paelen < 0) return -1;

    uint8_t sig[ED25519_SIG];
    if (ed25519_sign(sig, pae, (uint32_t)paelen, g_ed_sk) != 0) return -1;

    size_t payload_b64_cap = 4 * ((payload_len + 2) / 3) + 1;
    char* payload_b64 = (char*)malloc(payload_b64_cap);
    if (!payload_b64) return -1;
    b64enc(payload, payload_len, payload_b64);

    char sig_b64[4 * ((ED25519_SIG + 2) / 3) + 1];
    char keyid[2 * ED25519_PUBKEY + 1];
    b64enc(sig, ED25519_SIG, sig_b64);
    hexenc(g_ed_pk, ED25519_PUBKEY, keyid);

    size_t cap = payload_b64_cap + strlen(payload_type) + sizeof sig_b64 +
                 sizeof keyid + 160;
    char* envelope = (char*)malloc(cap);
    if (!envelope) { free(payload_b64); return -1; }
    int n = snprintf(envelope, cap,
        "{\"payloadType\":\"%s\",\"payload\":\"%s\","
        "\"signatures\":[{\"keyid\":\"%s\",\"sig\":\"%s\"}]}\n",
        payload_type, payload_b64, keyid, sig_b64);
    free(payload_b64);
    if (n < 0 || (size_t)n >= cap) { free(envelope); return -1; }
    *out = (uint8_t*)envelope;
    *out_len = (size_t)n;
    return 0;
}

typedef struct {
    int argc;
    char** argv;
    char* backing;
} sigil_export_args_t;

static void sigil_export_args_free(sigil_export_args_t* out)
{
    free(out->argv);
    free(out->backing);
    memset(out, 0, sizeof *out);
}

/* Modules cannot assume a newly exported host ABI is already installed. Keep
 * this small parser local so a freshly built module remains loadable by the
 * previous Jash binary during rolling upgrades. */
static int sigil_export_args_parse(const char* line, sigil_export_args_t* out)
{
    memset(out, 0, sizeof *out);
    if (!line || !*line) return 0;
    size_t len = strlen(line), bo = 0;
    out->backing = (char*)malloc(len + 1);
    out->argv = (char**)malloc(sizeof(char*) * (len / 2 + 2));
    if (!out->backing || !out->argv) {
        sigil_export_args_free(out);
        return -1;
    }

    const char* p = line;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\n') p++;
        if (!*p) break;
        out->argv[out->argc++] = out->backing + bo;
        int single = 0, dbl = 0;
        while (*p) {
            char c = *p;
            if (c == '\'' && !dbl) { single = !single; p++; continue; }
            if (c == '"' && !single) { dbl = !dbl; p++; continue; }
            if (!single && !dbl && (c == ' ' || c == '\t' || c == '\n')) break;
            if (!single && c == '\\' && p[1]) {
                out->backing[bo++] = p[1];
                p += 2;
                continue;
            }
            out->backing[bo++] = *p++;
        }
        if (single || dbl) {
            sigil_export_args_free(out);
            return -1;
        }
        out->backing[bo++] = '\0';
    }
    out->argv[out->argc] = NULL;
    return out->argc;
}

/* ── Helpers ────────────────────────────────────────────────────────── */

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* One definition shared with the governor; a stray .git file cannot split
 * the module ledger from the core's evidence scope. Identity stays pinned for
 * the lifetime of this module instance, including later changes of cwd. */
static const char* sigil_repo_key(void)
{
    static char key[18];
    if (!key[0]) jash_repo_identity(key);
    return key;
}

static void sigil_pin_ledger_identity(void)
{
    if (g_ledger.pinned) return;
    memset(&g_ledger, 0, sizeof g_ledger);

    const char* home = getenv("HOME");
    const char* repo = sigil_repo_key();
    snprintf(g_ledger.repo_key, sizeof g_ledger.repo_key, "%s",
             (repo && repo[0]) ? repo : "-");
    int home_supplied = home && home[0];
    if (home_supplied) {
        char resolved[PATH_MAX] = {0};
        if (realpath(home, resolved)) {
            size_t len = strlen(resolved);
            if (len < sizeof g_ledger.home)
                memcpy(g_ledger.home, resolved, len + 1);
        } else if (home[0] == '/') {
            size_t len = strlen(home);
            if (len < sizeof g_ledger.home)
                memcpy(g_ledger.home, home, len + 1);
        } else {
            char cwd[PATH_MAX];
            if (getcwd(cwd, sizeof cwd)) {
                size_t cwd_len = strlen(cwd), home_len = strlen(home);
                if (cwd_len + 1 + home_len < sizeof g_ledger.home) {
                    memcpy(g_ledger.home, cwd, cwd_len);
                    g_ledger.home[cwd_len] = '/';
                    memcpy(g_ledger.home + cwd_len + 1, home, home_len + 1);
                }
            }
        }
        g_ledger.has_home = g_ledger.home[0] != '\0';
    }
    if (g_ledger.has_home) {
        if (snprintf(g_ledger.jash_dir, sizeof g_ledger.jash_dir,
                     "%s/.jash", g_ledger.home) >=
            (int)sizeof g_ledger.jash_dir)
            goto invalid_identity;
        if (g_ledger.repo_key[0] != '-') {
            if (snprintf(g_ledger.chain, sizeof g_ledger.chain,
                         "%s/chain-%s.log", g_ledger.jash_dir,
                         g_ledger.repo_key) >= (int)sizeof g_ledger.chain ||
                snprintf(g_ledger.ratchet, sizeof g_ledger.ratchet,
                         "%s/ratchet-%s.key", g_ledger.jash_dir,
                         g_ledger.repo_key) >= (int)sizeof g_ledger.ratchet)
                goto invalid_identity;
        } else {
            if (snprintf(g_ledger.chain, sizeof g_ledger.chain,
                         "%s/chain.log", g_ledger.jash_dir) >=
                    (int)sizeof g_ledger.chain ||
                snprintf(g_ledger.ratchet, sizeof g_ledger.ratchet,
                         "%s/ratchet.key", g_ledger.jash_dir) >=
                    (int)sizeof g_ledger.ratchet)
                goto invalid_identity;
        }
        if (snprintf(g_ledger.legacy_ratchet,
                     sizeof g_ledger.legacy_ratchet, "%s/ratchet.key",
                     g_ledger.jash_dir) >=
                (int)sizeof g_ledger.legacy_ratchet ||
            snprintf(g_ledger.quarantine_dir,
                     sizeof g_ledger.quarantine_dir, "%s/quarantine",
                     g_ledger.jash_dir) >=
                (int)sizeof g_ledger.quarantine_dir)
            goto invalid_identity;
    } else if (!home_supplied) {
        snprintf(g_ledger.jash_dir, sizeof g_ledger.jash_dir, ".jash");
        snprintf(g_ledger.chain, sizeof g_ledger.chain, ".jash_chain.log");
        snprintf(g_ledger.ratchet, sizeof g_ledger.ratchet, ".jash_ratchet.key");
        snprintf(g_ledger.legacy_ratchet, sizeof g_ledger.legacy_ratchet,
                 ".jash_ratchet.key");
        snprintf(g_ledger.quarantine_dir, sizeof g_ledger.quarantine_dir,
                  ".jash/quarantine");
    } else {
        goto invalid_identity;
    }
    if (snprintf(g_ledger.keys_dir, sizeof g_ledger.keys_dir,
                 "%s/keys", g_ledger.jash_dir) >=
            (int)sizeof g_ledger.keys_dir ||
        snprintf(g_ledger.ed_seed, sizeof g_ledger.ed_seed,
                 "%s/ed25519.seed", g_ledger.keys_dir) >=
            (int)sizeof g_ledger.ed_seed ||
        snprintf(g_ledger.proof, sizeof g_ledger.proof,
                 "%s/session.jashproof", g_ledger.jash_dir) >=
            (int)sizeof g_ledger.proof ||
        snprintf(g_ledger.proof_dsse, sizeof g_ledger.proof_dsse,
                 "%s/session.jashproof.dsse", g_ledger.jash_dir) >=
            (int)sizeof g_ledger.proof_dsse ||
        snprintf(g_ledger.trusted_pub, sizeof g_ledger.trusted_pub,
                 "%s/trusted.pub", g_ledger.keys_dir) >=
            (int)sizeof g_ledger.trusted_pub ||
        snprintf(g_ledger.checkpoint, sizeof g_ledger.checkpoint,
                 "%s.scan", g_ledger.chain) >=
            (int)sizeof g_ledger.checkpoint ||
        snprintf(g_ledger.checkpoint_key, sizeof g_ledger.checkpoint_key,
                 "%s.ckptkey", g_ledger.chain) >=
            (int)sizeof g_ledger.checkpoint_key ||
        snprintf(g_ledger.transaction_lock, sizeof g_ledger.transaction_lock,
                 "%s.txnlock", g_ledger.chain) >=
            (int)sizeof g_ledger.transaction_lock)
        goto invalid_identity;
    g_ledger.pinned = 1;
    return;

invalid_identity:
    memset(&g_ledger, 0, sizeof g_ledger);
    g_ledger.pinned = 1;
}

static const char* sigil_identity_home(void)
{
    sigil_pin_ledger_identity();
    return g_ledger.has_home ? g_ledger.home : ".";
}

static const char* sigil_identity_jash_dir(void)
{
    sigil_pin_ledger_identity();
    return g_ledger.jash_dir;
}

static const char* sigil_identity_keys_dir(void)
{
    sigil_pin_ledger_identity();
    return g_ledger.keys_dir;
}

static const char* sigil_identity_ed_seed(void)
{
    sigil_pin_ledger_identity();
    return g_ledger.ed_seed;
}

static const char* sigil_identity_proof(void)
{
    sigil_pin_ledger_identity();
    return g_ledger.proof;
}

static const char* sigil_identity_proof_dsse(void)
{
    sigil_pin_ledger_identity();
    return g_ledger.proof_dsse;
}

static const char* sigil_identity_trusted_pub(void)
{
    sigil_pin_ledger_identity();
    return g_ledger.trusted_pub;
}

static const char* chain_path(void)
{
    sigil_pin_ledger_identity();
    return g_ledger.chain;
}

static FILE* chain_open(void)
{
    if (g_chain_fp) return g_chain_fp;
    sigil_pin_ledger_identity();
    if (!g_ledger.has_home) return NULL;
    mkdir(g_ledger.jash_dir, 0700);
    g_chain_fp = fopen(chain_path(), "a");
    return g_chain_fp;
}

static int chain_transaction_lock(void)
{
    sigil_pin_ledger_identity();
    int fd = open(g_ledger.transaction_lock, O_RDWR | O_CREAT, 0600);
    if (fd < 0) return -1;
    if (flock(fd, LOCK_EX) != 0) { close(fd); return -1; }
    return fd;
}

static void chain_transaction_unlock(int fd)
{
    if (fd < 0) return;
    (void)flock(fd, LOCK_UN);
    close(fd);
}

static int chain_stream_is_active(FILE* fp)
{
    struct stat opened, active;
    return fp && fstat(fileno(fp), &opened) == 0 &&
           stat(chain_path(), &active) == 0 &&
           opened.st_dev == active.st_dev && opened.st_ino == active.st_ino;
}

/* Transaction lock is always first. A long-lived stream may still reference a
 * quarantined inode, so it is rechecked under that lock before use. */
static FILE* chain_lock_active(int* transaction_fd)
{
    int guard = chain_transaction_lock();
    if (guard < 0) return NULL;
    FILE* fp = chain_open();
    if (!fp) {
        chain_transaction_unlock(guard);
        return NULL;
    }

    for (int attempt = 0; attempt < 3; attempt++) {
        if (!chain_stream_is_active(fp)) {
            if (fp == g_chain_fp) g_chain_fp = NULL;
            fclose(fp);
            fp = chain_open();
            if (!fp) break;
        }
        if (flock(fileno(fp), LOCK_EX) != 0) break;
        if (chain_stream_is_active(fp)) {
            *transaction_fd = guard;
            return fp;
        }
        (void)flock(fileno(fp), LOCK_UN);
        if (fp == g_chain_fp) g_chain_fp = NULL;
        fclose(fp);
        fp = chain_open();
        if (!fp) break;
    }
    chain_transaction_unlock(guard);
    return NULL;
}

/* Open and lock the active inode while the caller already owns txnlock. */
static FILE* chain_lock_active_held(void)
{
    FILE* fp = chain_open();
    if (!fp) return NULL;

    for (int attempt = 0; attempt < 3; attempt++) {
        if (!chain_stream_is_active(fp)) {
            if (fp == g_chain_fp) g_chain_fp = NULL;
            fclose(fp);
            fp = chain_open();
            if (!fp) break;
        }
        if (flock(fileno(fp), LOCK_EX) != 0) break;
        if (chain_stream_is_active(fp)) return fp;
        (void)flock(fileno(fp), LOCK_UN);
        if (fp == g_chain_fp) g_chain_fp = NULL;
        fclose(fp);
        fp = chain_open();
        if (!fp) break;
    }
    return NULL;
}

static void chain_unlock_active(FILE* fp, int transaction_fd)
{
    if (fp) (void)flock(fileno(fp), LOCK_UN);
    chain_transaction_unlock(transaction_fd);
}

/* ── Ratchet key persistence (S7) ──────────────────────────────────── */

static const char* ratchet_path(void)
{
    sigil_pin_ledger_identity();
    return g_ledger.ratchet;
}

static const char* legacy_ratchet_path(void)
{
    sigil_pin_ledger_identity();
    return g_ledger.legacy_ratchet;
}

static int sigil_fsync_parent(const char* path)
{
    char parent[PATH_MAX];
    if (snprintf(parent, sizeof parent, "%s", path) >= (int)sizeof parent) {
        errno = ENAMETOOLONG;
        return -1;
    }
    char* slash = strrchr(parent, '/');
    if (!slash) snprintf(parent, sizeof parent, ".");
    else if (slash == parent) slash[1] = '\0';
    else *slash = '\0';
    int fd = open(parent, O_RDONLY | O_DIRECTORY);
    if (fd < 0) return -1;
    int rc = fsync(fd);
    if (close(fd) != 0) rc = -1;
    return rc;
}

/* S3: ONE reader and ONE writer for the ratchet file, used by both the hot
 * path and the recovery verbs. JINV-099/100 were exactly this bug one level
 * down — two hand-written implementations of a single rule that drifted in
 * silence. A second parser for this file would be the same mistake again.
 *
 * Format: <key-hex-64> <advances> [<boundary-nonce-32hex>]
 * The third field is optional: a file in the old format reads as "no
 * boundary", degrading to previous behaviour instead of accusing falsely. */
static int ratchet_write_at(const char* path, const uint8_t key[32],
                            uint64_t advances, const char* nonce)
{
    char tmp[PATH_MAX];
    if (snprintf(tmp, sizeof tmp, "%s.tmp.%ld.%llu", path, (long)getpid(),
                 (unsigned long long)now_ns()) >= (int)sizeof tmp) {
        errno = ENAMETOOLONG;
        return -1;
    }
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return -1;
    FILE* f = fdopen(fd, "w");
    if (!f) { close(fd); unlink(tmp); return -1; }
    char hex[JASH_BLAKE3_HEX_LEN];
    jash_blake3_hex(key, hex);
    /* JINV-031: a POSICAO vai junto da chave. Sem ela, um log truncado passava
     * como intacto — a cadeia de hash de um prefixo e perfeitamente valida. */
    int wrote = nonce && nonce[0]
        ? fprintf(f, "%s %llu %s\n", hex, (unsigned long long)advances, nonce)
        : fprintf(f, "%s %llu\n", hex, (unsigned long long)advances);
    int ok = wrote >= 0 && fflush(f) == 0 && !ferror(f) && fsync(fd) == 0;
    if (fclose(f) != 0) ok = 0;
    if (!ok || rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return sigil_fsync_parent(path);
}

static int ratchet_read_at(const char* path, uint8_t key[32],
                           uint64_t* advances, char nonce[33])
{
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    char line[192];
    int ok = fgets(line, sizeof line, f) != NULL && strchr(line, '\n') != NULL &&
             fgetc(f) == EOF && !ferror(f);
    if (fclose(f) != 0) ok = 0;
    char key_hex[65] = {0};
    if (ok && strlen(line) >= 64) memcpy(key_hex, line, 64);
    if (!ok || strlen(line) < 67 || !sigil_hex64(key_hex) || line[64] != ' ')
        return -1;

    const char* count = line + 65;
    if (*count < '0' || *count > '9') return -1;
    errno = 0;
    char* end = NULL;
    unsigned long long adv = strtoull(count, &end, 10);
    if (errno == ERANGE || end == count) return -1;
    char nz[33] = {0};
    if (*end == '\n') {
        if (end[1] != '\0') return -1;
    } else {
        if (*end++ != ' ' || strlen(end) != 33 || end[32] != '\n') return -1;
        memcpy(nz, end, 32);
        nz[32] = '\0';
        for (int i = 0; i < 32; i++) {
            char c = nz[i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                  (c >= 'A' && c <= 'F')))
                return -1;
        }
    }
    for (int i = 0; i < 32; i++) {
        unsigned b = 0;
        if (sscanf(line + i*2, "%02x", &b) != 1) return -1;
        key[i] = (uint8_t)b;
    }
    if (advances) *advances = (uint64_t)adv;
    if (nonce) {
        if (nz[0]) memcpy(nonce, nz, sizeof nz);
        else nonce[0] = '\0';
    }
    return 0;
}

static int ratchet_save(void)
{
    const char* fault = getenv("JASH_SIGIL_TEST_RATCHET_FAIL");
    if (fault && strcmp(fault, "append") == 0) {
        errno = EIO;
        return -1;
    }
    return ratchet_write_at(ratchet_path(), g_ratchet_key,
                            g_ratchet_advances, g_ratchet_nonce);
}

static int ratchet_load(void)
{
    g_ratchet_advances = 0;
    g_ratchet_nonce[0] = '\0';
    if (ratchet_read_at(ratchet_path(), g_ratchet_key,
                        &g_ratchet_advances, g_ratchet_nonce) == 0)
        return 0;
    if (access(ratchet_path(), F_OK) == 0) return -1;
    FILE* rng = fopen("/dev/urandom", "rb");
    if (!rng || fread(g_ratchet_key, 1, 32, rng) != 32) {
        if (rng) fclose(rng);
        return -1;
    }
    fclose(rng);
    return ratchet_save();
}

/* Verification must judge the ledger and its witness at the same instant.
 * Long-lived shells cache the ratchet at module load; a peer can append after
 * that and make a healthy chain look forged unless every verifier refreshes
 * the witness while holding the chain lock shared with writers. */
static FILE* sigil_verify_snapshot_open_locked(void)
{
    for (int attempt = 0; attempt < 3; attempt++) {
        int descriptor = open(chain_path(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
        if (descriptor < 0) break;
        struct stat metadata;
        if (fstat(descriptor, &metadata) || !S_ISREG(metadata.st_mode)) {
            int error = errno ? errno : EINVAL; close(descriptor); errno = error; break;
        }
        FILE* chain = fdopen(descriptor, "r");
        if (!chain) { int error = errno; close(descriptor); errno = error; break; }
        if (flock(fileno(chain), LOCK_SH) != 0) {
            fclose(chain);
            break;
        }
        if (chain_stream_is_active(chain) &&
            ratchet_read_at(ratchet_path(), g_ratchet_key,
                            &g_ratchet_advances, g_ratchet_nonce) == 0) {
            return chain;
        }
        (void)flock(fileno(chain), LOCK_UN);
        fclose(chain);
    }
    return NULL;
}

static FILE* sigil_verify_snapshot_open(int* transaction_fd)
{
    int guard = chain_transaction_lock();
    if (guard < 0) return NULL;
    FILE* chain = sigil_verify_snapshot_open_locked();
    if (!chain) {
        chain_transaction_unlock(guard);
        return NULL;
    }
    *transaction_fd = guard;
    return chain;
}

static void sigil_verify_snapshot_close(FILE* chain, int transaction_fd)
{
    (void)flock(fileno(chain), LOCK_UN);
    fclose(chain);
    chain_transaction_unlock(transaction_fd);
}

/* ── The witness, computed in exactly ONE place ──────────────────────────
 * JINV-108: the boundary rule was implemented only inside the JSON verdict.
 * `sigil chain verify` — the one a human actually runs — kept comparing the
 * WHOLE chain against a ratchet that an honest `sigil ratchet rekey` had
 * just reset to 0, and convicted its own owner. Measured, before this fix:
 *
 *   echo one; sigil ratchet rekey; echo after1; echo after2; sigil chain verify
 *     -> "ENTRADAS FORJADAS — a cadeia tem 6 entradas, mas o ratchet so
 *         avancou 4 vezes"                                    (honest history)
 *
 * A verifier that convicts the honest case is worse than no verifier: it
 * teaches the operator that red means nothing. The rule had drifted because
 * it existed twice; so it now exists once and both callers borrow it. */
typedef struct {
    uint64_t bnd_count;   /* sealed boundaries seen so far          */
    uint64_t after_bnd;   /* entries since the most recent boundary */
    char     nonce[33];   /* nonce carried by that boundary         */
} sigil_boundary_t;

static void sigil_boundary_track(sigil_boundary_t* b, const char* cmd)
{
    if (strncmp(cmd, SIGIL_BOUNDARY_MARK, strlen(SIGIL_BOUNDARY_MARK)) == 0) {
        b->bnd_count++;
        b->after_bnd = 0;
        b->nonce[0] = '\0';
        const char* np = strstr(cmd, " nonce=");
        if (np) sscanf(np, " nonce=%32s", b->nonce);
    } else {
        b->after_bnd++;
    }
}

/* A boundary line can be TYPED by anyone; the nonce stored beside the key
 * (0600, never in the log alone) cannot. So an unrecognised nonce is not an
 * error — it simply buys no leniency: we fall back to judging the whole
 * chain, which is the strict reading. Forgetting to check it would let an
 * attacker erase history by pasting one line. */
static uint64_t sigil_witness_entries(const sigil_boundary_t* b,
                                      uint64_t checked, const char** scope)
{
    if (b->bnd_count > 0 && g_ratchet_nonce[0] &&
        strcmp(b->nonce, g_ratchet_nonce) == 0) {
        if (scope) *scope = "current_segment";
        return b->after_bnd;
    }
    if (scope) *scope = "whole_chain";
    return checked;
}

static const char* sigil_scan_recovery_reason(const sigil_chain_scan_t* scan)
{
    if (scan->s7_entries > 0 && scan->fragments > 0 &&
        scan->malformed == 0 && scan->legacy_entries == 0)
        return "corrupted_newline_fragment";
    if (scan->malformed > 0 || scan->legacy_entries > 0 ||
        (scan->physical_lines > 0 && scan->s7_entries == 0 &&
         scan->unattested == 0))
        return "unknown_malformed_chain";

    const char* k = sigil_repo_key();
    if (k && k[0] != '-' && scan->physical_lines > 0 &&
        access(ratchet_path(), F_OK) != 0)
        return access(legacy_ratchet_path(), F_OK) == 0
            ? "legacy_global_ratchet_scope_mismatch"
            : "missing_scoped_ratchet";
    if (access(ratchet_path(), F_OK) == 0) {
        uint8_t key[32];
        uint64_t advances = 0;
        char nonce[33] = {0};
        if (ratchet_read_at(ratchet_path(), key, &advances, nonce) != 0)
            return "malformed_ratchet";
        memset(key, 0, sizeof key);
        if (scan->boundaries > 0 && scan->bnd_nonce[0] &&
            strcmp(scan->bnd_nonce, nonce) != 0)
            return "ratchet_replacement_incomplete";
    }
    return NULL;
}

static const char* sigil_absent_chain_recovery_reason(void)
{
    if (access(chain_path(), F_OK) == 0 || access(ratchet_path(), F_OK) != 0)
        return NULL;
    uint8_t key[32];
    uint64_t advances = 0;
    char nonce[33] = {0};
    if (ratchet_read_at(ratchet_path(), key, &advances, nonce) != 0)
        return "malformed_ratchet";
    memset(key, 0, sizeof key);
    return advances != 0 || nonce[0]
        ? "orphaned_ratchet_without_chain" : NULL;
}

/* ── JINV-122: resumable scan checkpoint ────────────────────────────────
 *
 * Loading the module folded the whole ledger, so starting a shell cost
 * O(history) — a 50MB chain re-parsed on every prompt. The fold is pure over
 * sigil_chain_scan_t, so its value at a byte offset is a legitimate resume
 * point, for the same reason the ledger can be appended to at all. We persist
 * that value beside the chain and next time fold only the delta.
 *
 * It is a CACHE, never evidence, and the difference is enforced, not merely
 * promised:
 *   - `sigil chain verify`, `quarantine status` and every proof path call
 *     sigil_scan_chain(), which always re-reads from byte zero. Only
 *     chain_resume()'s startup tally consults the checkpoint.
 *   - it is MACed with a key derived from the scoped ratchet FILE (mode
 *     0600), so a writer who cannot read the ratchet cannot forge one; and
 *     any ratchet write — advance, rekey, adopt, scope change — changes the
 *     key and silently invalidates it. There is no stale-cache window.
 *   - it carries the accusatory counters forward. A chain already convicted
 *     of fragments or malformed lines stays convicted across restarts; the
 *     cache can never launder a verdict, only skip re-deriving it.
 *   - deleting the file is always safe and always correct.
 *
 * What it necessarily trusts is the prefix it already folded: you cannot
 * re-verify N bytes in constant time. That trust is bounded by three cheap
 * checks — the file must not have shrunk below the offset, the byte before
 * the offset must be a newline, and the first line after the seam must
 * continue the sequence — so a truncation, a rewrite that shifts bytes, or a
 * replaced file all fall back to the full scan. Tampering that survives all
 * three still faces `sigil chain verify`, which never reads this file.
 * JASH_SIGIL_CKPT=0 turns the whole thing off. */

#define SIGIL_CKPT_MAGIC "jash-chain-ckpt v3"
/* v2 → v3 (2026-09-19): the line classifier learned the pre-context
 * level-LOG wire shape (zero hash + lifecycle tag + rc + cmd, no
 * cwd=/repo=/dur=/sess=) as `unattested` instead of `malformed`. A v2
 * checkpoint caches the OLD verdict (malformed>0) with a valid seam+MAC,
 * so resuming from it would fail-close every append despite the fixed
 * scanner. Bumping the magic forces one full rescan per chain; the new
 * verdict is then stored under v3. */

/* Minimum chain growth before the checkpoint is re-stored. The ckpt is a pure
 * resume cache — MAC'd, seam-checked, re-derivable from the chain — so it
 * does not need a write per appended command; a stale one just makes the
 * next scan re-read a few KB past the seam. */
#define SIGIL_CKPT_MIN_ADVANCE 16384

static int ckpt_enabled(void)
{
    const char* e = getenv("JASH_SIGIL_CKPT");
    return !(e && (strcmp(e, "0") == 0 || strcmp(e, "off") == 0));
}

static const char* ckpt_path(void)
{
    sigil_pin_ledger_identity();
    return g_ledger.checkpoint[0] ? g_ledger.checkpoint : NULL;
}

/* The key comes from the ratchet file's bytes rather than g_ratchet_key
 * because chain_resume() scans BEFORE it decides whether to load the ratchet
 * at all — in memory the key is still zeroes here. Reading the file instead
 * is also what buys the free invalidation described above. No ratchet, no
 * checkpoint: an unscoped or absent ratchet is exactly the unsettled state
 * that deserves a full scan. */
static const char* ckpt_key_path(void)
{
    sigil_pin_ledger_identity();
    return g_ledger.checkpoint_key[0] ? g_ledger.checkpoint_key : NULL;
}

/* JINV-169: the key that authenticates the scan checkpoint must be STABLE.
 * Deriving it from the ratchet was self-defeating: the ratchet is FORWARD-
 * SECURE and is rewritten after every logged command, so by the time anything
 * came to read the checkpoint, the key that MACed it no longer existed. Every
 * startup therefore rejected its own cache ("mac") and re-scanned the entire
 * chain — the O(history)→O(delta) resume of JINV-122 never once took effect
 * on a chain that was being written to, which is every real chain. Measured
 * on a 100 MB / 405k-entry chain: ~7 s for a bare `echo hi`, on EVERY command,
 * in every terminal.
 *
 * A dedicated per-chain secret (32 random bytes, 0600, created once) is
 * exactly as strong here — the threat is a forged checkpoint steering the
 * scanner past real content, and both keys sit in the same trust domain —
 * but it does not move underneath the feature it protects. Losing or
 * rotating it is self-healing: the MAC fails once, the chain is scanned in
 * full, and a fresh checkpoint is written under the new key. */
static int ckpt_key_derive(uint8_t out[32])
{
    const char* kp = ckpt_key_path();
    if (!kp) return -1;
    static const uint8_t domain[32] = "jash-chain-ckpt-v2";
    uint8_t raw[32];

    for (int attempt = 0; attempt < 3; attempt++) {
        FILE* f = fopen(kp, "rb");
        if (f) {
            size_t n = fread(raw, 1, sizeof raw, f);
            fclose(f);
            if (n != sizeof raw) return -1;   /* truncated key → no cache */
            jash_blake3_keyed(domain, raw, sizeof raw, out);
            return 0;
        }
        /* Exclusive create: a key another process is mid-write must never be
         * half-read into service. Losing the race just means re-reading it. */
        int fd = open(kp, O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd < 0) continue;
        FILE* rng = fopen("/dev/urandom", "rb");
        int ok = (rng && fread(raw, 1, sizeof raw, rng) == sizeof raw);
        if (rng) fclose(rng);
        if (ok) ok = (write(fd, raw, sizeof raw) == (ssize_t)sizeof raw);
        close(fd);
        if (!ok) { unlink(kp); return -1; }
        jash_blake3_keyed(domain, raw, sizeof raw, out);
        return 0;
    }
    return -1;
}

static int ckpt_body(char* buf, size_t cap, const char* chain,
                     long long off, const sigil_chain_scan_t* s)
{
    int n = snprintf(buf, cap,
        SIGIL_CKPT_MAGIC "\n"
        "chain %s\n"
        "offset %lld\n"
        "seam_seq %llu\n"
        "physical_lines %llu\n"
        "s7_entries %llu\n"
        "legacy_entries %llu\n"
        "fragments %llu\n"
        "malformed %llu\n"
        "first_fragment_offset %lld\n"
        "entries_after_fragment %llu\n"
        "boundaries %llu\n"
        "entries_after_boundary %llu\n"
        "bnd_prev_advances %llu\n"
        "bnd_prev_entries %llu\n"
        "unattested %llu\n"
        "tag_lines %llu\n"
        "bnd_nonce %s\n"
        "bnd_mode %s\n"
        "last_s7 %s\n",
        chain, off,
        (unsigned long long)s->last_seq,
        (unsigned long long)s->physical_lines,
        (unsigned long long)s->s7_entries,
        (unsigned long long)s->legacy_entries,
        (unsigned long long)s->fragments,
        (unsigned long long)s->malformed,
        s->first_fragment_offset,
        (unsigned long long)s->entries_after_fragment,
        (unsigned long long)s->boundaries,
        (unsigned long long)s->entries_after_boundary,
        (unsigned long long)s->bnd_prev_advances,
        (unsigned long long)s->bnd_prev_entries,
        (unsigned long long)s->unattested,
        (unsigned long long)s->tag_lines,
        s->bnd_nonce[0] ? s->bnd_nonce : "-",
        s->bnd_mode[0]  ? s->bnd_mode  : "-",
        s->last_s7);
    if (n < 0 || (size_t)n >= cap) return -1;
    return n;
}

static int ckpt_u64(const char* line, const char* key, uint64_t* out)
{
    size_t k = strlen(key);
    if (strncmp(line, key, k) != 0 || line[k] != ' ') return 0;
    *out = strtoull(line + k + 1, NULL, 10);
    return 1;
}

static int ckpt_i64(const char* line, const char* key, long long* out)
{
    size_t k = strlen(key);
    if (strncmp(line, key, k) != 0 || line[k] != ' ') return 0;
    *out = strtoll(line + k + 1, NULL, 10);
    return 1;
}

static int ckpt_str(const char* line, const char* key, char* out, size_t cap)
{
    size_t k = strlen(key);
    if (strncmp(line, key, k) != 0 || line[k] != ' ') return 0;
    const char* v = line + k + 1;
    if (strcmp(v, "-") == 0) { out[0] = '\0'; return 1; }
    snprintf(out, cap, "%s", v);
    return 1;
}

/* The seam check. `off` is only usable if it is a line boundary AND the line
 * that starts there continues the ledger's sequence. A rewrite that shifted
 * old bytes lands the seam mid-record or on the wrong seq, and we rescan. */
static int ckpt_seam_ok(const char* chain, long long off, uint64_t seam_seq,
                        long long size)
{
    if (off < 0 || off > size) return 0;
    if (off == 0) return 1;
    FILE* f = fopen(chain, "r");
    if (!f) return 0;
    if (fseeko(f, (off_t)(off - 1), SEEK_SET) != 0) { fclose(f); return 0; }
    int prev = fgetc(f);
    if (prev != '\n') { fclose(f); return 0; }
    if (off == size) { fclose(f); return 1; }   /* nothing new to seam onto */
    char line[SIGIL_LINE_MAX];
    int ok = 0;
    if (fgets(line, sizeof line, f)) {
        unsigned long long next = 0;
        if (sscanf(line, "%llu", &next) == 1 && next == (unsigned long long)seam_seq + 1)
            ok = 1;
    }
    fclose(f);
    return ok;
}

/* JINV-169 diagnostics: a checkpoint that is silently rejected looks exactly
 * like one that works — the only visible difference is the wall clock. Name
 * the gate that refused, so "the cache is on" can never again be assumed. */
static void ckpt_dbg(const char* why)
{
    if (getenv("JASH_SIGIL_CKPT_DEBUG"))
        fprintf(stderr, "[ckpt] reject: %s\n", why);
}

static int ckpt_load(const char* chain, sigil_chain_scan_t* scan, long long* off_out)
{
    if (!ckpt_enabled()) return -1;
    const char* cp = ckpt_path();
    if (!cp) { ckpt_dbg("no ckpt path"); return -1; }
    uint8_t key[32];
    if (ckpt_key_derive(key) != 0) { ckpt_dbg("key derive (no scoped ratchet?)"); return -1; }

    struct stat st;
    if (stat(chain, &st) != 0) { ckpt_dbg("stat chain"); return -1; }

    size_t cap = SIGIL_LINE_MAX + 2048;
    char* buf = malloc(cap);
    if (!buf) return -1;
    FILE* f = fopen(cp, "r");
    if (!f) { free(buf); return -1; }
    size_t n = fread(buf, 1, cap - 1, f);
    int overflow = (fgetc(f) != EOF);
    fclose(f);
    buf[n] = '\0';
    if (n == 0 || overflow) { free(buf); return -1; }

    /* Layout: "mac <hex>\n" then the MACed body. Header-first so the split is
     * unambiguous without scanning backwards for a trailing field. */
    char* body = strchr(buf, '\n');
    if (!body || strncmp(buf, "mac ", 4) != 0) { free(buf); return -1; }
    *body++ = '\0';
    char machex[JASH_BLAKE3_HEX_LEN];
    uint8_t mac[32];
    jash_blake3_keyed(key, (const uint8_t*)body, strlen(body), mac);
    jash_blake3_hex(mac, machex);
    if (strcmp(buf + 4, machex) != 0) { free(buf); ckpt_dbg("mac"); return -1; }

    sigil_chain_scan_t s;
    memset(&s, 0, sizeof s);
    s.first_fragment_offset = -1;
    long long off = -1;
    char chain_seen[PATH_MAX] = {0};
    int magic = 0;
    uint64_t v = 0;
    char* save = NULL;
    for (char* line = strtok_r(body, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        if (strcmp(line, SIGIL_CKPT_MAGIC) == 0)              { magic = 1; continue; }
        if (ckpt_str(line, "chain", chain_seen, sizeof chain_seen)) continue;
        if (ckpt_u64(line, "offset", &v))                     { off = (long long)v; continue; }
        if (ckpt_u64(line, "seam_seq", &s.last_seq))          continue;
        if (ckpt_u64(line, "physical_lines", &s.physical_lines)) continue;
        if (ckpt_u64(line, "s7_entries", &s.s7_entries))      continue;
        if (ckpt_u64(line, "legacy_entries", &s.legacy_entries)) continue;
        if (ckpt_u64(line, "fragments", &s.fragments))        continue;
        if (ckpt_u64(line, "malformed", &s.malformed))        continue;
        if (ckpt_i64(line, "first_fragment_offset", &s.first_fragment_offset)) continue;
        if (ckpt_u64(line, "entries_after_fragment", &s.entries_after_fragment)) continue;
        if (ckpt_u64(line, "boundaries", &s.boundaries))      continue;
        if (ckpt_u64(line, "entries_after_boundary", &s.entries_after_boundary)) continue;
        if (ckpt_u64(line, "bnd_prev_advances", &s.bnd_prev_advances)) continue;
        if (ckpt_u64(line, "bnd_prev_entries", &s.bnd_prev_entries)) continue;
        if (ckpt_u64(line, "unattested", &s.unattested))      continue;
        if (ckpt_u64(line, "tag_lines", &s.tag_lines))        continue;
        if (ckpt_str(line, "bnd_nonce", s.bnd_nonce, sizeof s.bnd_nonce)) continue;
        if (ckpt_str(line, "bnd_mode", s.bnd_mode, sizeof s.bnd_mode)) continue;
        if (ckpt_str(line, "last_s7", s.last_s7, sizeof s.last_s7)) continue;
    }
    free(buf);

    if (!magic || off < 0 || strcmp(chain_seen, chain) != 0) {
        ckpt_dbg(!magic ? "magic" : (off < 0 ? "offset" : "chain path mismatch"));
        return -1;
    }
    if (!ckpt_seam_ok(chain, off, s.last_seq, (long long)st.st_size)) {
        ckpt_dbg("seam");
        return -1;
    }

    *scan = s;
    *off_out = off;
    return 0;
}

static void ckpt_store(const char* chain, const sigil_chain_scan_t* s, long long off)
{
    if (!ckpt_enabled() || off < 0) return;
    const char* cp = ckpt_path();
    if (!cp) return;
    uint8_t key[32];
    if (ckpt_key_derive(key) != 0) return;   /* no scoped ratchet, no cache */

    size_t cap = SIGIL_LINE_MAX + 2048;
    char* body = malloc(cap);
    if (!body) return;
    int n = ckpt_body(body, cap, chain, off, s);
    if (n < 0) { free(body); return; }

    uint8_t mac[32];
    char machex[JASH_BLAKE3_HEX_LEN];
    jash_blake3_keyed(key, (const uint8_t*)body, (size_t)n, mac);
    jash_blake3_hex(mac, machex);

    /* Written through a temp file: a half-written checkpoint that still
     * parsed would be the one way this cache could lie. */
    char tmp[PATH_MAX];
    if ((size_t)snprintf(tmp, sizeof tmp, "%s.tmp", cp) < sizeof tmp) {
        FILE* f = fopen(tmp, "w");
        if (f) {
            int ok = (fprintf(f, "mac %s\n%s", machex, body) > 0);
            if (fflush(f) != 0) ok = 0;
            fclose(f);
            chmod(tmp, 0600);
            if (!ok || rename(tmp, cp) != 0) unlink(tmp);
        }
    }
    free(body);
}

/* The startup tally, and the only reader of the checkpoint. */
static int sigil_scan_chain_resumable(const char* path, sigil_chain_scan_t* scan)
{
    memset(scan, 0, sizeof *scan);
    scan->first_fragment_offset = -1;
    long long start = 0;
    int resumed = (ckpt_load(path, scan, &start) == 0);
    if (getenv("JASH_SIGIL_CKPT_DEBUG"))
        fprintf(stderr, "[ckpt] resumed=%d start=%lld path=%s\n",
                resumed, start, path ? path : "(null)");
    if (!resumed) {
        memset(scan, 0, sizeof *scan);
        scan->first_fragment_offset = -1;
        start = 0;
    }
    long long end = -1;
    if (sigil_scan_chain_from(path, scan, start, &end) != 0) return -1;
    /* Store only when the scan covered real ground (batching — see
     * SIGIL_CKPT_MIN_ADVANCE). A shrink (end < start, e.g. a chain trim)
     * always stores: resuming past the new EOF would never seam. And when
     * nothing grew and the checkpoint was already valid, leave the file
     * alone, so a shell that only reads costs zero writes. */
    if (!resumed || end < start || end - start >= SIGIL_CKPT_MIN_ADVANCE)
        ckpt_store(path, scan, end);
    return 0;
}

/* ── Chain resume + S7 migration ────────────────────────────────────── */

static void chain_resume(void)
{
    memset(g_prev_hash, 0, sizeof(g_prev_hash));
    memset(g_ratchet_key, 0, sizeof(g_ratchet_key));
    g_seq = 0;
    g_ratchet_advances = 0;
    g_append_block_reason = NULL;
    g_active_entries = 0;
    g_active_fragments = 0;
    g_first_fragment_offset = -1;
    g_entries_after_fragment = 0;
    g_active_unattested = 0;
    g_active_malformed = 0;
    g_active_legacy = 0;
    g_append_block_warned = 0;
    g_invalid_v1_artifact = 0;
    g_v1_entries = 0;
    g_v1_fragments = 0;
    g_pending_v1_migration = 0;
    g_read_only_pending = 0;
    g_last_command_read_only = 0;
    g_native_read_only_argc = -1;

    sigil_chain_scan_t scan;
    int scanned = (sigil_scan_chain_resumable(chain_path(), &scan) == 0);
    if (scanned) {
        /* JINV-103: record the tally on every path, not only the blocked one,
         * so `quarantine status` never reports zeros for a file it did read. */
        g_active_entries = scan.s7_entries;
        g_active_fragments = scan.fragments;
        g_first_fragment_offset = scan.first_fragment_offset;
        g_entries_after_fragment = scan.entries_after_fragment;
        g_active_unattested = scan.unattested;
        g_active_malformed = scan.malformed;
        g_active_legacy = scan.legacy_entries;
    }
    if (!scanned) {
        /* Fresh-pair recovery publishes a zero-position witness before the
         * empty chain, so a crash can leave this recoverable staging state but
         * can never expose a chain without its witness. Anything other than a
         * pristine witness beside an absent chain is ambiguous and blocks. */
        if (access(ratchet_path(), F_OK) == 0) {
            if (ratchet_load() != 0)
                g_append_block_reason = "malformed_ratchet";
            else
                g_append_block_reason =
                    sigil_absent_chain_recovery_reason();
        }
    } else if (scan.physical_lines == 0 && access(ratchet_path(), F_OK) == 0) {
        /* `quarantine apply` deliberately starts an empty ledger beside a
         * fresh scoped ratchet. That pair is an initialized S7 chain, not an
         * ambiguous legacy file. */
        if (ratchet_load() != 0) g_append_block_reason = "malformed_ratchet";
    } else if (scan.s7_entries == 0 && scan.legacy_entries > 0 &&
               scan.fragments == 0 && scan.malformed == 0) {
        /* A v1 migration is allowed only when every physical line has the
         * recognized legacy shape. It is deferred until the first append so
         * `quarantine status` remains byte-for-byte read-only. */
        g_pending_v1_migration = 1;
    } else {
        if (scan.s7_entries > 0 && scan.fragments > 0 &&
            scan.malformed == 0 && scan.legacy_entries == 0)
            g_append_block_reason = "corrupted_newline_fragment";
        /* JINV-103: unattested (level-LOG) records are NOT corruption, so they
         * must not brick the chain. `s7_entries == 0` is fatal only when the
         * file yields nothing intelligible at all; a LOG-only chain is a real,
         * common state (the user simply never ran at level SIGN) and is left
         * to the scoped-ratchet checks below to adjudicate. */
        else if ((scan.s7_entries == 0 && scan.unattested == 0) ||
                 scan.malformed > 0 || scan.legacy_entries > 0)
            g_append_block_reason = "unknown_malformed_chain";

        const char* k = sigil_repo_key();
        if (!g_append_block_reason && k && k[0] != '-' &&
            access(ratchet_path(), F_OK) != 0) {
            g_append_block_reason =
                access(legacy_ratchet_path(), F_OK) == 0
                    ? "legacy_global_ratchet_scope_mismatch"
                    : "missing_scoped_ratchet";
        }

        if (!g_append_block_reason) {
            uint64_t seq = 0;
            char ts_str[32], hexhash[65];
            if (sscanf(scan.last_s7, "%llu %31s %64s",
                       (unsigned long long*)&seq, ts_str, hexhash) == 3 &&
                sigil_hex64(hexhash)) {
                g_seq = seq + 1;
                (void)hexdec(hexhash, g_prev_hash, 32);
            }
            if (ratchet_load() != 0)
                g_append_block_reason = "malformed_ratchet";
            else if (scan.boundaries > 0 && scan.bnd_nonce[0] &&
                      strcmp(scan.bnd_nonce, g_ratchet_nonce) != 0)
                g_append_block_reason = "ratchet_replacement_incomplete";
            else {
                uint64_t witnessed =
                    scan.boundaries > 0 && scan.bnd_nonce[0] &&
                    strcmp(scan.bnd_nonce, g_ratchet_nonce) == 0
                        ? scan.entries_after_boundary : scan.s7_entries;
                if (g_ratchet_advances != witnessed)
                    g_append_block_reason = "ratchet_position_mismatch";
            }
        }
    }

    char v1[PATH_MAX];
    if (sigil_path_suffix(v1, sizeof v1, chain_path(), ".v1") != 0) {
        if (!g_append_block_reason)
            g_append_block_reason = "identity_sidecar_path_too_long";
        return;
    }
    sigil_chain_scan_t old;
    if (sigil_scan_chain(v1, &old) == 0 && old.tag_lines > 0) {
        g_invalid_v1_artifact = 1;
        g_v1_entries = old.s7_entries + old.legacy_entries;
        g_v1_fragments = old.fragments;
    }
}

/* JINV-112: uma linha serve de ANCORA so se for atestada. Um registro de
 * nivel LOG (hash = 64 zeros, escrito por c/module/sigil.c) e estruturalmente
 * valido e nao e fragmento — mas ancorar nele poe zeros em g_prev_hash e forka
 * a cadeia de verdade. `chain_resume` ja acertava isso via scan.last_s7 +
 * sigil_hex64(); `chain_reread_tip` checava apenas strlen()==64 e aceitava os
 * zeros. Duas implementacoes da mesma decisao, uma empobrecida — exatamente o
 * que o comentario de chain_write_entry logo abaixo alerta, e a raiz do
 * JINV-112. Uma so, ou o desvio volta. */
static int sigil_line_can_anchor(const char* line)
{
    uint64_t seq = 0; char ts_str[32]; char hexhash[65];
    if (sscanf(line, "%llu %31s %64s",
               (unsigned long long*)&seq, ts_str, hexhash) != 3) return 0;
    if (strlen(hexhash) != 64) return 0;
    return sigil_hex64(hexhash) && strspn(hexhash, "0") != 64;
}

/* Re-read ONLY the tip (seq + prev hash) from the file. Called at append
 * time under flock: another process — or a core jash_sigil_attest() inside
 * THIS process — may have extended the chain since init, and a stale g_seq/
 * g_prev_hash would fork the chain (duplicate seq, wrong prev → verify gap). */
static void chain_reread_tip(void)
{
    FILE* f = fopen(chain_path(), "r");
    if (!f) return;
    /* JINV-101: same rule as chain_resume — the tip is the last WELL-FORMED
     * entry. Reading the last physical line meant a trailing fragment made
     * this return early, leaving g_seq/g_prev_hash stale and forking the
     * chain on the next append (duplicate seq, wrong prev → verify gap).
     *
     * JINV-169: ...but finding it with a FORWARD scan cost O(chain) on EVERY
     * append, under flock. At 100 MB / 405k entries that measured ~7 s for a
     * bare `echo hi` — and because the chain only ever grows, the per-command
     * price grows with it (a 200-command battery took 481 s). The tip is at
     * the END of the file, so seek there and walk BACKWARDS in blocks: the
     * common case is one 64 KB read. Earlier blocks are visited only when the
     * tail holds nothing well-formed, so a pathological chain still resolves
     * to the same entry — it just stops being the happy path. Identical rule,
     * identical result, O(1) instead of O(n). */
    char last[SIGIL_LINE_MAX] = {0};
    enum { TIP_BLOCK = 1 << 16 };   /* > SIGIL_LINE_MAX, so a line always fits */
    char* buf = (char*)malloc(TIP_BLOCK + 1);
    if (!buf) { fclose(f); return; }
    off_t end = (fseeko(f, 0, SEEK_END) == 0) ? ftello(f) : -1;

    if (end < 0) {
        /* Unseekable stream: fall back to the forward scan. A stale tip forks
         * the chain, so correctness outranks the speed win here. */
        rewind(f);
        while (fgets(buf, TIP_BLOCK, f)) {
            if (sigil_line_is_fragment(buf)) continue;
            if (!sigil_line_can_anchor(buf)) continue;   /* JINV-112 */
            snprintf(last, sizeof last, "%s", buf);
        }
    }

    while (end > 0 && !last[0]) {
        off_t start = end > (off_t)TIP_BLOCK ? end - (off_t)TIP_BLOCK : 0;
        size_t want = (size_t)(end - start);
        if (fseeko(f, start, SEEK_SET) != 0) break;
        size_t got = fread(buf, 1, want, f);
        if (got == 0) break;
        buf[got] = '\0';

        size_t pos = got;
        while (pos > 0) {
            size_t line_end = pos;
            if (buf[line_end - 1] == '\n') line_end--;      /* drop terminator */
            size_t line_start = line_end;
            while (line_start > 0 && buf[line_start - 1] != '\n') line_start--;
            /* A block that does not begin at offset 0 starts mid-line; that
             * head is incomplete and must not be judged — the next read will
             * cover it whole. */
            if (line_start == 0 && start != 0) break;
            size_t len = line_end - line_start;
            if (len > 0 && len < sizeof last) {
                memcpy(last, buf + line_start, len);
                last[len] = '\0';
                /* JINV-112: nao-fragmento NAO basta — a ponta tem que poder
                 * ancorar. Uma linha de nivel LOG passa no teste de fragmento
                 * e poria zeros em g_prev_hash. */
                if (!sigil_line_is_fragment(last) && sigil_line_can_anchor(last))
                    break;                                 /* ← this is the tip */
                last[0] = '\0';
            }
            pos = line_start;
        }
        if (start == 0) break;                    /* whole file examined */

        /* The next block must END at this block's first newline, so the entry
         * straddling the boundary lands in it complete. */
        const char* nl = (const char*)memchr(buf, '\n', got);
        off_t next_end = nl ? start + (off_t)(nl - buf) + 1 : start;
        end = (next_end < end) ? next_end : start;          /* must shrink */
    }

    free(buf);
    fclose(f);
    if (!last[0]) {
        /* A quarantine transaction can replace the inode with a deliberately
         * empty active ledger. Reopening the path is not enough: the cached
         * process-local tip belongs to the quarantined inode too. */
        memset(g_prev_hash, 0, sizeof g_prev_hash);
        g_seq = 0;
        return;
    }
    uint64_t seq = 0;
    char ts_str[32];
    char hexhash[65];
    if (sscanf(last, "%llu %31s %64s",
               (unsigned long long*)&seq, ts_str, hexhash) == 3
        && sigil_line_can_anchor(last)) {          /* JINV-112: uma porta so */
        g_seq = seq + 1;
        for (int i = 0; i < 32; i++) {
            unsigned byte = 0;
            sscanf(hexhash + i*2, "%02x", &byte);
            g_prev_hash[i] = (uint8_t)byte;
        }
    }
}

/* ── Chain entry ────────────────────────────────────────────────────── */

/* Seals ONE entry into `fp` with an EXPLICIT key/counter pair, advancing
 * both. Every writer in this module goes through here — the hot path and the
 * S3 boundary writer alike.
 *
 * A second hand-written copy of this arithmetic is not a hypothetical risk:
 * it is precisely how JINV-100 happened (writer bounded cmd at -plen-1, the
 * verifier at -plen, and every command landing on that boundary hashed a
 * different length on each side and read as FORGED). One implementation, or
 * the drift comes back. */
static void chain_write_entry(FILE* fp, const char* cmd, int rc,
                              uint8_t key[32], uint64_t* advances)
{
    uint64_t ts = now_ns();

    /* JINV-099: escape FIRST, then hash and write the same bytes, so the
     * cmd field on the line is exactly what the hash covers. */
    char cmd_esc[SIGIL_CMD_ESC_MAX];
    sigil_wire_escape(cmd, cmd_esc, sizeof cmd_esc);

    /* payload: ts_ns(8B BE) || rc(4B LE) || cmd(escaped) */
    uint8_t payload[4096];
    size_t  plen = 0;
    for (int i = 7; i >= 0; i--) { payload[plen++] = (uint8_t)(ts >> (i*8)); }
    payload[plen++] = (uint8_t)(rc & 0xFF);
    payload[plen++] = (uint8_t)((rc >> 8) & 0xFF);
    payload[plen++] = (uint8_t)((rc >> 16) & 0xFF);
    payload[plen++] = (uint8_t)((rc >> 24) & 0xFF);
    size_t clen = strlen(cmd_esc);
    /* JINV-100: this bound MUST equal the verifier's, byte for byte. */
    if (clen > SIGIL_PAYLOAD_CMD_MAX(plen)) clen = SIGIL_PAYLOAD_CMD_MAX(plen);
    cmd_esc[clen] = '\0';
    if (clen) { memcpy(payload + plen, cmd_esc, clen); plen += clen; }

    /* h_n = BLAKE3(h_{n-1} || payload) */
    uint8_t new_hash[32];
    jash_blake3_chain(g_prev_hash, payload, plen, new_hash);
    memcpy(g_prev_hash, new_hash, 32);

    char hexhash[JASH_BLAKE3_HEX_LEN];
    jash_blake3_hex(new_hash, hexhash);

    /* tag_n = BLAKE3_keyed(k_n, h_n); k_{n+1} = BLAKE3(k_n || "r") */
    uint8_t tag[32];
    jash_blake3_keyed(key, new_hash, 32, tag);
    jash_blake3_ratchet(key);
    (*advances)++;                              /* JINV-031 */

    char taghex[JASH_BLAKE3_HEX_LEN];
    jash_blake3_hex(tag, taghex);
    memset(tag, 0, 32);

    /* ADR-0051 W1: context fields BEFORE cmd= — display-only (the BLAKE3
     * payload above is ts||rc||cmd, so chain verification is unchanged). */
    char cwd_buf[512];
    if (!getcwd(cwd_buf, sizeof cwd_buf)) cwd_buf[0] = '\0';
    for (char* p2 = cwd_buf; *p2; p2++) if (*p2 == ' ') *p2 = '_';
    const char* sess_env = getenv("JASH_SESSION");
    char sess_buf[64];
    snprintf(sess_buf, sizeof sess_buf, "%s", (sess_env && *sess_env) ? sess_env : "-");
    for (char* p2 = sess_buf; *p2; p2++) if (*p2 == ' ') *p2 = '_';
    long dur_ms = (long)((now_ns() - g_ts_start_ns) / 1000000ULL);

    fprintf(fp, "%llu %llu.%09llu %s tag=%s rc=%d cwd=%s repo=%s dur=%ld sess=%s cmd=%s\n",
            (unsigned long long)g_seq,
            (unsigned long long)(ts / 1000000000ULL),
            (unsigned long long)(ts % 1000000000ULL),
            hexhash, taghex, rc,
            cwd_buf, sigil_repo_key(), dur_ms, sess_buf,
            cmd_esc);            /* JINV-099: same bytes the hash covers */
}

typedef struct { int write_attempted, chain_synced, ratchet_published; uint64_t sequence; } sigil_record_observation_t;

static int chain_append_observed(const char* cmd, int rc, sigil_record_observation_t* observation)
{
    memset(observation, 0, sizeof *observation);
    if (g_pending_v1_migration) {
        char v1[PATH_MAX];
        int v1_ok = sigil_path_suffix(v1, sizeof v1, chain_path(), ".v1") == 0;
        if (!v1_ok || access(v1, F_OK) == 0 || rename(chain_path(), v1) != 0) {
            /* JINV-103: name what actually failed. The chain is fine here —
             * the .v1 slot was occupied or the rename errored. Calling that
             * "unknown_malformed_chain" sends the user hunting corruption
             * that does not exist. */
            g_append_block_reason = "v1_migration_failed";
        } else {
            g_pending_v1_migration = 0;
            memset(g_prev_hash, 0, sizeof g_prev_hash);
            memset(g_ratchet_key, 0, sizeof g_ratchet_key);
            g_seq = 0;
            g_ratchet_advances = 0;
            if (ratchet_load() != 0)
                g_append_block_reason = "ratchet_publish_failed";
        }
    }
    if (g_append_block_reason) {
        const char* explicit_level = getenv("JASH_SIGIL");
        if (!g_append_block_warned && explicit_level && atoi(explicit_level) > 0) {
            fprintf(stderr, "sigil append blocked: %s; run `sigil quarantine status --json`\n",
                    g_append_block_reason);
            g_append_block_warned = 1;
        }
        return 1;
    }
    int transaction_fd = -1;
    FILE* fp = chain_lock_active(&transaction_fd);
    if (!fp) {
        fprintf(stderr, "sigil append blocked: cannot open active chain: %s\n",
                strerror(errno));
        return 1;
    }

    /* serialize read-tip + append with every other writer (other jash
     * processes, core attest lines) so seq and prev-hash never fork */
    (void)fflush(fp);
    chain_reread_tip();
    /* the ratchet is shared state too: a core attest in this same process
     * (or another process) advances it on disk — reload key+position or this
     * append would reuse a stale key and read as forged (JINV-031). */
    if (ratchet_load() != 0) {
        g_append_block_reason = "malformed_ratchet";
        chain_unlock_active(fp, transaction_fd);
        return 1;
    }
    sigil_chain_scan_t current;
    memset(&current, 0, sizeof current);
    current.first_fragment_offset = -1;
    if (sigil_scan_chain_resumable(chain_path(), &current) != 0 ||
        sigil_scan_recovery_reason(&current)) {
        g_append_block_reason = "active_chain_changed";
        chain_unlock_active(fp, transaction_fd);
        return 1;
    }
    uint64_t witnessed =
        current.boundaries > 0 && current.bnd_nonce[0] &&
        strcmp(current.bnd_nonce, g_ratchet_nonce) == 0
            ? current.entries_after_boundary : current.s7_entries;
    if (g_ratchet_advances != witnessed) {
        g_append_block_reason = "ratchet_position_mismatch";
        fprintf(stderr,
                "sigil append blocked: ratchet_position_mismatch; "
                "run `sigil quarantine status --json`\n");
        chain_unlock_active(fp, transaction_fd);
        return 1;
    }

    observation->write_attempted = 1;
    observation->sequence = g_seq;
    chain_write_entry(fp, cmd, rc, g_ratchet_key, &g_ratchet_advances);
    if (ferror(fp) || fflush(fp) != 0 || fsync(fileno(fp)) != 0) {
        g_append_block_reason = "chain_sync_failed";
        chain_unlock_active(fp, transaction_fd);
        return 1;
    }
    observation->chain_synced = 1;
    if (ratchet_save() != 0) {
        g_append_block_reason = "ratchet_publish_failed";
        chain_unlock_active(fp, transaction_fd);
        return 1;
    }
    chain_unlock_active(fp, transaction_fd);
    observation->ratchet_published = 1;
    g_seq++;
    return 0;
}

static void chain_append(const char* cmd, int rc) {
    sigil_record_observation_t observation;
    (void)chain_append_observed(cmd, rc, &observation);
}

/* ── Explicit recovery: classify, quarantine, then start clean ─────── */

static int cmd_chain_verify_json(jmod_writer_t w, void* ctx);
static int cmd_chain_verify_json_locked(jmod_writer_t w, void* ctx,
                                        int held_transaction_fd);

#include "sigil_verify_observed.inc"

static int sigil_chain_verify_result(int rc)
{
    /* A failed verifier has just established that appending to this ledger is
     * unsafe. Preserve the evidence and ratchet position instead of recording
     * the verifier command onto the chain it rejected. Absence of a ledger
     * is NOT such evidence: `sigil chain verify` on a fresh HOME (or before
     * this session's first append lands at top-level eval end) must report
     * failure without poisoning the session, or the just-run commands would
     * never be recorded at all. */
    if (rc != 0 && !g_append_block_reason &&
        access(chain_path(), F_OK) == 0)
        g_append_block_reason = "chain_verification_failed";
    return rc;
}

/* Conclusive native verdicts alone can authorize evidence-preserving
 * quarantine; unavailable snapshots and I/O errors stay non-destructive. */
static const char* sigil_verifier_recovery_reason(int transaction_fd)
{
    sigil_verify_observation_t observation;
    (void)sigil_verify_observe(&observation, transaction_fd);
    const char* failure = observation.failure;
    if (failure && (!strcmp(failure, "hash_mismatch") ||
                    !strcmp(failure, "log_truncated") ||
                    !strcmp(failure, "entries_forged"))) return failure;
    return NULL;
}

static int sigil_file_sha256(const char* path, char out[JASH_SHA256_HEX_LEN])
{
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long sz = ftell(f);
    if (sz < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }
    uint8_t* data = (uint8_t*)malloc(sz > 0 ? (size_t)sz : 1);
    if (!data) { fclose(f); return -1; }
    if (sz > 0 && fread(data, 1, (size_t)sz, f) != (size_t)sz) {
        free(data); fclose(f); return -1;
    }
    fclose(f);
    uint8_t digest[JASH_SHA256_DIGEST_LEN];
    jash_sha256(data, (size_t)sz, digest);
    jash_sha256_hex(digest, out);
    free(data);
    return 0;
}

static const char* sigil_basename(const char* path)
{
    const char* slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static int sigil_path_suffix(char* out, size_t cap, const char* path,
                             const char* suffix)
{
    size_t path_len = strlen(path), suffix_len = strlen(suffix);
    if (path_len > cap || suffix_len >= cap - path_len) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(out, path, path_len);
    memcpy(out + path_len, suffix, suffix_len + 1);
    return 0;
}

static int quarantine_dir_path(char* out, size_t cap)
{
    sigil_pin_ledger_identity();
    return snprintf(out, cap, "%s", g_ledger.quarantine_dir) < (int)cap ? 0 : -1;
}

static int sigil_fsync_dir(const char* path)
{
    int fd = open(path, O_RDONLY | O_DIRECTORY);
    if (fd < 0) return -1;
    int rc = fsync(fd);
    if (close(fd) != 0) rc = -1;
    return rc;
}

/* Recover only an unambiguous trailing fragment. The active prefix remains
 * byte-identical and the ratchet is not moved: fragment bytes never advanced
 * it. Middle corruption is deliberately refused by the caller. */
static int quarantine_tail_suffix(const char* original, const char* reason,
                                  uint64_t entries, uint64_t fragments,
                                  long long retained_offset,
                                  char* manifest_out, size_t manifest_cap)
{
    char original_sha[JASH_SHA256_HEX_LEN], suffix_sha[JASH_SHA256_HEX_LEN];
    char manifest_tmp[PATH_MAX] = "";
    char quarantined[PATH_MAX] = "";
    char qdir[PATH_MAX] = "";
    int manifest_published = 0;
    if (retained_offset < 0) return -1;

    int source = open(original, O_RDWR);
    if (source < 0) return -1;
    if (flock(source, LOCK_EX) != 0) { close(source); return -1; }
    struct stat st, active;
    sigil_chain_scan_t fresh;
    memset(&fresh, 0, sizeof fresh);
    fresh.first_fragment_offset = -1;
    if (fstat(source, &st) != 0 || stat(original, &active) != 0 ||
        st.st_dev != active.st_dev || st.st_ino != active.st_ino ||
        retained_offset >= (long long)st.st_size ||
        sigil_file_sha256(original, original_sha) != 0 ||
        sigil_scan_chain(original, &fresh) != 0 ||
        fresh.first_fragment_offset != retained_offset ||
        fresh.entries_after_fragment != 0 || fresh.fragments == 0 ||
        fresh.malformed != 0 || fresh.legacy_entries != 0) {
        (void)flock(source, LOCK_UN); close(source); return -1;
    }
    entries = fresh.s7_entries;
    fragments = fresh.fragments;

    if (quarantine_dir_path(qdir, sizeof qdir) != 0) goto unchanged;
    if (mkdir(qdir, 0700) != 0 && errno != EEXIST) goto unchanged;
    uint64_t detected = now_ns();
    unsigned long long sec = (unsigned long long)(detected / 1000000000ULL);
    unsigned long long nsec = (unsigned long long)(detected % 1000000000ULL);
    if (snprintf(quarantined, sizeof quarantined,
                 "%s/%s-%llu.%09llu-%.127s", qdir, reason, sec, nsec,
                 sigil_basename(original)) >= (int)sizeof quarantined)
        goto unchanged;
    int suffix = open(quarantined, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (suffix < 0 || lseek(source, (off_t)retained_offset, SEEK_SET) < 0) {
        if (suffix >= 0) close(suffix);
        goto unchanged;
    }
    char buf[8192];
    int ok = 1;
    for (;;) {
        ssize_t nr = read(source, buf, sizeof buf);
        if (nr < 0) { ok = 0; break; }
        if (nr == 0) break;
        ssize_t at = 0;
        while (at < nr) {
            ssize_t nw = write(suffix, buf + at, (size_t)(nr - at));
            if (nw <= 0) { ok = 0; break; }
            at += nw;
        }
        if (!ok) break;
    }
    if (ok && fsync(suffix) != 0) ok = 0;
    if (close(suffix) != 0) ok = 0;
    if (!ok || sigil_file_sha256(quarantined, suffix_sha) != 0) {
        goto unchanged;
    }

    if (snprintf(manifest_out, manifest_cap,
                 "%s/%s-%llu.%09llu-%.127s.manifest.json", qdir, reason,
                 sec, nsec, sigil_basename(original)) >= (int)manifest_cap ||
        snprintf(manifest_tmp, sizeof manifest_tmp, "%s.tmp.%ld",
                 manifest_out, (long)getpid()) >= (int)sizeof manifest_tmp)
        goto unchanged;
    FILE* mf = fopen(manifest_tmp, "w");
    if (!mf) goto unchanged;
    char eorig[PATH_MAX * 2], equar[PATH_MAX * 2];
    jash_json_esc(original, eorig, sizeof eorig);
    jash_json_esc(quarantined, equar, sizeof equar);
    int wrote = fprintf(mf,
        "{\"original\":\"%s\",\"quarantined\":\"%s\","
        "\"reason\":\"%s\",\"entry_count\":%llu,"
        "\"fragment_count\":%llu,\"detected_at\":%llu,"
        "\"original_sha256\":\"%s\",\"sha256\":\"%s\","
        "\"retained_offset\":%lld,\"tail_salvaged\":true,"
        "\"ratchet_quarantined\":null}\n",
        eorig, equar, reason, (unsigned long long)entries,
        (unsigned long long)fragments, (unsigned long long)detected,
        original_sha, suffix_sha, retained_offset) > 0;
    const char* fault = getenv("JASH_SIGIL_TEST_QUARANTINE_FAIL");
    if (fault && strcmp(fault, "suffix-manifest") == 0) wrote = 0;
    if (wrote && (fflush(mf) != 0 || fsync(fileno(mf)) != 0)) wrote = 0;
    int manifest_close_ok = fclose(mf) == 0;
    if (!wrote || !manifest_close_ok || rename(manifest_tmp, manifest_out) != 0)
        goto unchanged;
    manifest_tmp[0] = '\0';
    manifest_published = 1;
    if (sigil_fsync_dir(qdir) != 0) goto unchanged;

    if ((fault && strcmp(fault, "suffix-truncate") == 0) ||
        ftruncate(source, (off_t)retained_offset) != 0)
        goto unchanged;

    /* From this point the quarantined suffix is the only copy of the removed
     * bytes. No later error path may unlink it or its commit record. */
    int durable = fsync(source) == 0;
    if (fault && strcmp(fault, "suffix-fsync") == 0) durable = 0;
    unlink(ckpt_path());
    int dirs_durable = sigil_fsync_dir(qdir) == 0 &&
                       sigil_fsync_parent(original) == 0;
    if (fault && strcmp(fault, "suffix-dir-fsync") == 0) dirs_durable = 0;
    (void)flock(source, LOCK_UN);
    close(source);
    return durable && dirs_durable ? 0 : 1;

unchanged:
    if (manifest_tmp[0]) unlink(manifest_tmp);
    if (manifest_published) unlink(manifest_out);
    if (quarantined[0]) unlink(quarantined);
    if (qdir[0]) {
        (void)sigil_fsync_dir(qdir);
        (void)sigil_fsync_parent(original);
    }
    manifest_out[0] = '\0';
    (void)flock(source, LOCK_UN);
    close(source);
    return -1;
}

static int quarantine_move_one(const char* original, const char* reason,
                               uint64_t entries, uint64_t fragments,
                               int move_scoped_ratchet,
                               char* manifest_out, size_t manifest_cap)
{
    char before[JASH_SHA256_HEX_LEN];
    char quarantined[PATH_MAX] = "", ratchet_quarantined[PATH_MAX] = "";
    char manifest_tmp[PATH_MAX] = "";
    char qdir[PATH_MAX] = "";
    int source = -1, chain_moved = 0, ratchet_moved = 0;
    int manifest_created = 0, manifest_published = 0;
    FILE* mf = NULL;
    const char* quarantine_fault = getenv("JASH_SIGIL_TEST_QUARANTINE_FAIL");
    manifest_out[0] = '\0';

    source = open(original, O_RDONLY);
    if (source < 0 || flock(source, LOCK_EX) != 0) goto rollback;
    struct stat opened, active;
    if (fstat(source, &opened) != 0 || stat(original, &active) != 0 ||
        opened.st_dev != active.st_dev || opened.st_ino != active.st_ino ||
        sigil_file_sha256(original, before) != 0)
        goto rollback;

    if (quarantine_dir_path(qdir, sizeof qdir) != 0) goto rollback;
    if (mkdir(qdir, 0700) != 0 && errno != EEXIST) goto rollback;

    uint64_t detected = now_ns();
    unsigned long long sec = (unsigned long long)(detected / 1000000000ULL);
    unsigned long long nsec = (unsigned long long)(detected % 1000000000ULL);
    if (snprintf(quarantined, sizeof quarantined,
                 "%s/%s-%llu.%09llu-%.127s", qdir, reason, sec, nsec,
                 sigil_basename(original)) >= (int)sizeof quarantined)
        goto rollback;

    const char* scoped_ratchet = ratchet_path();
    if (move_scoped_ratchet && access(scoped_ratchet, F_OK) == 0) {
        if (snprintf(ratchet_quarantined, sizeof ratchet_quarantined,
                     "%s/%s-%llu.%09llu-%.127s", qdir, reason, sec, nsec,
                     sigil_basename(scoped_ratchet)) >=
            (int)sizeof ratchet_quarantined)
            goto rollback;
        if (rename(scoped_ratchet, ratchet_quarantined) != 0) goto rollback;
        ratchet_moved = 1;
        if (quarantine_fault &&
            strcmp(quarantine_fault, "crash-after-ratchet-move") == 0)
            _exit(86);
    }
    /* Removing the witness first makes every interrupted namespace fail in the
     * already-recoverable direction: chain without witness. The old order
     * removed the chain, re-read the whole quarantined file, and left an
     * advanced witness permanently orphaned when the process died. A same-FS
     * rename preserves the locked inode and the digest measured above. */
    if (rename(original, quarantined) != 0) goto rollback;
    chain_moved = 1;

    if (snprintf(manifest_out, manifest_cap,
                 "%s/%s-%llu.%09llu-%.127s.manifest.json", qdir, reason,
                 sec, nsec, sigil_basename(original)) >= (int)manifest_cap ||
        snprintf(manifest_tmp, sizeof manifest_tmp, "%s.tmp.%ld",
                 manifest_out, (long)getpid()) >= (int)sizeof manifest_tmp)
        goto rollback;
    int manifest_fd = open(manifest_tmp, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (manifest_fd < 0) goto rollback;
    manifest_created = 1;
    mf = fdopen(manifest_fd, "w");
    if (!mf) { close(manifest_fd); goto rollback; }
    char eorig[PATH_MAX * 2], equar[PATH_MAX * 2];
    char eratchet[PATH_MAX * 2], eglobal[PATH_MAX * 2];
    jash_json_esc(original, eorig, sizeof eorig);
    jash_json_esc(quarantined, equar, sizeof equar);
    jash_json_esc(ratchet_quarantined, eratchet, sizeof eratchet);
    jash_json_esc(legacy_ratchet_path(), eglobal, sizeof eglobal);
    int wrote = fprintf(mf,
            "{\"original\":\"%s\",\"quarantined\":\"%s\","
            "\"reason\":\"%s\",\"entry_count\":%llu,"
            "\"fragment_count\":%llu,\"detected_at\":%llu,"
            "\"sha256\":\"%s\",\"ratchet_quarantined\":",
            eorig, equar, reason,
            (unsigned long long)entries, (unsigned long long)fragments,
            (unsigned long long)detected, before) >= 0;
    if (ratchet_quarantined[0]) wrote = wrote && fprintf(mf, "\"%s\"", eratchet) >= 0;
    else wrote = wrote && fputs("null", mf) != EOF;
    wrote = wrote && fprintf(mf,
            ",\"legacy_global_ratchet\":\"%s\","
            "\"legacy_global_ratchet_exists\":%s}\n",
            eglobal,
            access(legacy_ratchet_path(), F_OK) == 0 ? "true" : "false") >= 0;
    const char* fault = getenv("JASH_SIGIL_TEST_MANIFEST_FAIL");
    if (fault && strcmp(fault, "write") == 0) wrote = 0;
    if (wrote && fflush(mf) != 0) wrote = 0;
    if (wrote && fsync(fileno(mf)) != 0) wrote = 0;
    int close_ok = fclose(mf) == 0;
    mf = NULL;
    if (fault && strcmp(fault, "close") == 0) close_ok = 0;
    if (!wrote || !close_ok) goto rollback;
    if (rename(manifest_tmp, manifest_out) != 0) goto rollback;
    manifest_created = 0;
    manifest_published = 1;

    /* The manifest is a commit record: publish success only after both rename
     * directories and the manifest directory entry are durable. */
    int dirs_durable = sigil_fsync_dir(qdir) == 0 &&
                       sigil_fsync_parent(original) == 0;
    if (quarantine_fault && strcmp(quarantine_fault, "full-dir-fsync") == 0)
        dirs_durable = 0;
    if (!dirs_durable)
        goto rollback;

    (void)flock(source, LOCK_UN);
    close(source);
    return 0;

rollback:
    if (mf) fclose(mf);
    if (manifest_created) unlink(manifest_tmp);
    const char* rollback_fault = getenv("JASH_SIGIL_TEST_QUARANTINE_FAIL");
    int force_rollback_failure = rollback_fault &&
                                  strcmp(rollback_fault, "full-rollback") == 0;
    int fail_chain_restore = force_rollback_failure ||
        (rollback_fault && strcmp(rollback_fault, "rollback-chain") == 0);
    int fail_ratchet_restore = force_rollback_failure ||
        (rollback_fault &&
         (strcmp(rollback_fault, "rollback-ratchet") == 0 ||
          strcmp(rollback_fault, "rollback-ratchet-requarantine") == 0));
    int ratchet_restored = !ratchet_moved;
    int chain_restored = !chain_moved;
    /* Restore the chain first. Restoring the ratchet while the chain remains
     * quarantined creates an active witness for an absent ledger. */
    if (chain_moved && !fail_chain_restore &&
        rename(quarantined, original) == 0)
        chain_restored = 1;
    if (chain_restored && ratchet_moved && !fail_ratchet_restore &&
        rename(ratchet_quarantined, ratchet_path()) == 0)
        ratchet_restored = 1;
    if (chain_restored && !ratchet_restored) {
        /* Keep the pair together in quarantine when ratchet restoration fails. */
        int fail_requarantine = rollback_fault &&
            (strcmp(rollback_fault, "rollback-requarantine") == 0 ||
             strcmp(rollback_fault, "rollback-ratchet-requarantine") == 0);
        if (!fail_requarantine && rename(original, quarantined) == 0)
            chain_restored = 0;
        else if (rename(ratchet_quarantined, ratchet_path()) == 0)
            ratchet_restored = 1;
    }

    /* A published manifest is removed only when every moved recovery artifact
     * returned to active state. If rollback fails, the quarantined files are
     * the sole copies and both they and any valid commit record must remain. */
    if (chain_restored && ratchet_restored && manifest_published)
        unlink(manifest_out);
    if (qdir[0]) {
        (void)sigil_fsync_dir(qdir);
        (void)sigil_fsync_parent(original);
    }
    if (chain_restored && ratchet_restored) manifest_out[0] = '\0';
    if (source >= 0) {
        (void)flock(source, LOCK_UN);
        close(source);
    }
    int pair_quarantined = chain_moved && !chain_restored &&
                           (!ratchet_moved || !ratchet_restored);
    if (pair_quarantined) return 1;
    if (!ratchet_restored) return 2;
    if (!chain_restored) return 2;
    return -1;
}

static int quarantine_move_orphan_ratchet(const char* reason,
                                           char* manifest_out,
                                           size_t manifest_cap)
{
    const char* original = ratchet_path();
    char before[JASH_SHA256_HEX_LEN], after[JASH_SHA256_HEX_LEN];
    char quarantined[PATH_MAX] = "", manifest_tmp[PATH_MAX] = "";
    char qdir[PATH_MAX] = "";
    int source = -1, moved = 0, manifest_published = 0;
    FILE* mf = NULL;
    manifest_out[0] = '\0';

    source = open(original, O_RDONLY);
    if (source < 0 || flock(source, LOCK_EX) != 0) goto rollback;
    struct stat opened, active;
    if (fstat(source, &opened) != 0 || stat(original, &active) != 0 ||
        opened.st_dev != active.st_dev || opened.st_ino != active.st_ino ||
        sigil_file_sha256(original, before) != 0)
        goto rollback;
    if (quarantine_dir_path(qdir, sizeof qdir) != 0) goto rollback;
    if (mkdir(qdir, 0700) != 0 && errno != EEXIST) goto rollback;

    uint64_t detected = now_ns();
    unsigned long long sec = (unsigned long long)(detected / 1000000000ULL);
    unsigned long long nsec = (unsigned long long)(detected % 1000000000ULL);
    if (snprintf(quarantined, sizeof quarantined,
                 "%s/%s-%llu.%09llu-%.127s", qdir, reason, sec, nsec,
                 sigil_basename(original)) >= (int)sizeof quarantined ||
        snprintf(manifest_out, manifest_cap,
                 "%s/%s-%llu.%09llu-%.127s.manifest.json", qdir, reason,
                 sec, nsec, sigil_basename(original)) >= (int)manifest_cap ||
        snprintf(manifest_tmp, sizeof manifest_tmp, "%s.tmp.%ld",
                 manifest_out, (long)getpid()) >= (int)sizeof manifest_tmp)
        goto rollback;

    int manifest_fd = open(manifest_tmp, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (manifest_fd < 0) goto rollback;
    mf = fdopen(manifest_fd, "w");
    if (!mf) { close(manifest_fd); goto rollback; }
    char eorig[PATH_MAX * 2], equar[PATH_MAX * 2], eglobal[PATH_MAX * 2];
    jash_json_esc(original, eorig, sizeof eorig);
    jash_json_esc(quarantined, equar, sizeof equar);
    jash_json_esc(legacy_ratchet_path(), eglobal, sizeof eglobal);
    int wrote = fprintf(mf,
        "{\"original\":\"%s\",\"quarantined\":\"%s\","
        "\"reason\":\"%s\",\"entry_count\":0,\"fragment_count\":0,"
        "\"detected_at\":%llu,\"sha256\":\"%s\","
        "\"ratchet_quarantined\":\"%s\","
        "\"legacy_global_ratchet\":\"%s\","
        "\"legacy_global_ratchet_exists\":%s}\n",
        eorig, equar, reason, (unsigned long long)detected, before, equar,
        eglobal, access(legacy_ratchet_path(), F_OK) == 0 ? "true" : "false") >= 0;
    if (wrote && (fflush(mf) != 0 || fsync(fileno(mf)) != 0)) wrote = 0;
    int close_ok = fclose(mf) == 0;
    mf = NULL;
    if (!wrote || !close_ok) goto rollback;

    if (rename(original, quarantined) != 0) goto rollback;
    moved = 1;
    if (sigil_file_sha256(quarantined, after) != 0 ||
        strcmp(before, after) != 0 || sigil_fsync_dir(qdir) != 0 ||
        sigil_fsync_parent(original) != 0)
        goto rollback;
    if (rename(manifest_tmp, manifest_out) != 0) goto rollback;
    manifest_tmp[0] = '\0';
    manifest_published = 1;
    if (sigil_fsync_dir(qdir) != 0) goto rollback;

    (void)flock(source, LOCK_UN);
    close(source);
    return 0;

rollback:
    if (mf) fclose(mf);
    if (moved && rename(quarantined, original) == 0) moved = 0;
    if (!moved && manifest_published) unlink(manifest_out);
    if (manifest_tmp[0]) unlink(manifest_tmp);
    if (qdir[0]) {
        (void)sigil_fsync_dir(qdir);
        (void)sigil_fsync_parent(original);
    }
    if (source >= 0) {
        (void)flock(source, LOCK_UN);
        close(source);
    }
    if (!moved) manifest_out[0] = '\0';
    return moved ? 1 : -1;
}

static void quarantine_list_manifests(jmod_writer_t w, void* ctx)
{
    char qdir[PATH_MAX];
    if (quarantine_dir_path(qdir, sizeof qdir) != 0) return;
    DIR* dir = opendir(qdir);
    int first = 1;
    if (dir) {
        struct dirent* ent;
        while ((ent = readdir(dir)) != NULL) {
            if (!strstr(ent->d_name, ".manifest.json")) continue;
            char path[PATH_MAX], escaped[PATH_MAX * 2];
            if (snprintf(path, sizeof path, "%s/%s", qdir, ent->d_name) >=
                (int)sizeof path)
                continue;
            jash_json_esc(path, escaped, sizeof escaped);
            jash_writef(w, ctx, "%s\"%s\"", first ? "" : ",", escaped);
            first = 0;
        }
        closedir(dir);
    }
}

static void quarantine_list_recovery_artifacts(jmod_writer_t w, void* ctx)
{
    char qdir[PATH_MAX];
    if (quarantine_dir_path(qdir, sizeof qdir) != 0) return;
    DIR* dir = opendir(qdir);
    int first = 1;
    if (dir) {
        struct dirent* ent;
        while ((ent = readdir(dir)) != NULL) {
            if (ent->d_name[0] == '.' ||
                strstr(ent->d_name, ".manifest.json") ||
                strstr(ent->d_name, ".tmp."))
                continue;
            char path[PATH_MAX], escaped[PATH_MAX * 2];
            if (snprintf(path, sizeof path, "%s/%s", qdir, ent->d_name) >=
                (int)sizeof path)
                continue;
            jash_json_esc(path, escaped, sizeof escaped);
            jash_writef(w, ctx, "%s\"%s\"", first ? "" : ",", escaped);
            first = 0;
        }
        closedir(dir);
    }
}

static int sigil_create_fresh_pair(void)
{
    const char* fault = getenv("JASH_SIGIL_TEST_QUARANTINE_FAIL");
    if (fault && strcmp(fault, "fresh-pair") == 0) { errno = EIO; return -1; }
    struct stat metadata;
    if (!lstat(chain_path(), &metadata)) { errno = EEXIST; return -1; }
    if (errno != ENOENT) return -1;
    if (!lstat(ratchet_path(), &metadata)) { errno = EEXIST; return -1; }
    if (errno != ENOENT) return -1;
    uint8_t key[32];
    FILE* rng = fopen("/dev/urandom", "rb");
    if (!rng || fread(key, 1, sizeof key, rng) != sizeof key) {
        if (rng) fclose(rng);
        memset(key, 0, sizeof key);
        return -1;
    }
    if (fclose(rng)) { memset(key, 0, sizeof key); return -1; }
    /* Publish the pristine witness first. If power fails before the empty
     * chain rename, chain_resume recognizes advances=0/no nonce as staging and
     * the first append completes the empty side. The inverse order could expose
     * a ledger with no witness and is therefore never used. */
    if (ratchet_write_at(ratchet_path(), key, 0, "") != 0) {
        int error = errno ? errno : EIO;
        memset(key, 0, sizeof key);
        int removed = !unlink(ratchet_path()) || errno == ENOENT;
        int restored = !sigil_fsync_parent(ratchet_path());
        errno = error;
        return removed && restored ? -1 : -2;
    }
    int chain_created = 0;
    if (fault && strcmp(fault, "fresh-chain") == 0) goto rollback_pair;

    int fd = open(chain_path(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) goto rollback_pair;
    chain_created = 1;
    int durable = fsync(fd) == 0;
    if (close(fd) != 0) durable = 0;
    if (durable) durable = sigil_fsync_parent(chain_path()) == 0;
    if (!durable) goto rollback_pair;
    memset(key, 0, sizeof key);
    return 0;

rollback_pair:
    memset(key, 0, sizeof key);
    {
        int error = errno ? errno : EIO;
        int removed_chain = !chain_created || !unlink(chain_path()) || errno == ENOENT;
        int removed_witness = !unlink(ratchet_path()) || errno == ENOENT;
        int restored = !sigil_fsync_parent(chain_path());
        errno = error;
        /* -1 establishes rollback; -2 says that publication was attempted
         * and cleanup could not establish the final namespace/durability. */
        return removed_chain && removed_witness && restored ? -1 : -2;
    }
}

static int cmd_quarantine_status(jmod_writer_t w, void* ctx)
{
    /* The status command is observational: do not attest the command or add
     * a shutdown sentinel to the ledger it is inspecting. */
    g_read_only_pending = 1;
    g_native_read_only_argc = -1;
    sigil_chain_scan_t scan;
    memset(&scan, 0, sizeof scan);
    scan.first_fragment_offset = -1;
    sigil_chain_scan_t old;
    memset(&old, 0, sizeof old);
    old.first_fragment_offset = -1;
    int transaction_fd = chain_transaction_lock();
    FILE* active = NULL;
    int snapshot_ok = transaction_fd >= 0;
    int chain_exists = access(chain_path(), F_OK) == 0;
    if (snapshot_ok && chain_exists) {
        active = fopen(chain_path(), "r");
        snapshot_ok = active && flock(fileno(active), LOCK_SH) == 0 &&
                      chain_stream_is_active(active) &&
                      sigil_scan_chain(chain_path(), &scan) == 0;
    }
    char v1[PATH_MAX];
    int v1_path_ok = sigil_path_suffix(v1, sizeof v1, chain_path(), ".v1") == 0;
    int invalid_v1 = snapshot_ok && v1_path_ok &&
                     sigil_scan_chain(v1, &old) == 0 && old.tag_lines > 0;
    const char* active_reason = snapshot_ok
        ? (chain_exists ? sigil_scan_recovery_reason(&scan)
                        : sigil_absent_chain_recovery_reason())
        : "snapshot_unavailable";
    int witness_available = 0;
    int witness_mismatch = 0;
    uint64_t witness_entries = 0;
    uint64_t ratchet_advances = 0;
    const char* witness_scope = "unavailable";
    if (snapshot_ok && chain_exists && access(ratchet_path(), F_OK) == 0) {
        uint8_t key[32] = {0};
        char nonce[33] = {0};
        int ratchet_ok = ratchet_read_at(ratchet_path(), key,
                                         &ratchet_advances, nonce) == 0;
        memset(key, 0, sizeof key);
        if (ratchet_ok) {
            witness_available = 1;
            int current_segment = scan.boundaries > 0 && scan.bnd_nonce[0] &&
                                  strcmp(scan.bnd_nonce, nonce) == 0;
            witness_scope = current_segment ? "current_segment" : "whole_chain";
            witness_entries = current_segment ? scan.entries_after_boundary
                                              : scan.s7_entries;
            witness_mismatch = witness_entries != ratchet_advances;
        }
    }
    /* The normal status path remains structural and cheap, but append can also
     * be blocked by a perfectly parseable ledger whose independent ratchet
     * position differs. Report that exact fail-closed condition here so the
     * prescribed diagnostic command does not return `active:null`. */
    if (!active_reason && witness_mismatch)
        active_reason = "ratchet_position_mismatch";

    jash_writes(w, ctx, "{\"active\":");
    if (active_reason) {
        char path[PATH_MAX * 2];
        jash_json_esc(
            strcmp(active_reason, "orphaned_ratchet_without_chain") == 0
                ? ratchet_path() : chain_path(),
            path, sizeof path);
        jash_writef(w, ctx,
            "{\"path\":\"%s\",\"reason\":\"%s\","
            "\"entry_count\":%llu,\"fragment_count\":%llu}",
            path, active_reason,
            (unsigned long long)scan.s7_entries,
            (unsigned long long)scan.fragments);
    } else {
        jash_writes(w, ctx, "null");
    }
    /* JINV-103: always publish what the scanner counted. A verdict the user
     * cannot check is just an assertion; these five numbers are the evidence
     * behind `reason`, and they are emitted whether or not append is blocked. */
    jash_writef(w, ctx,
        ",\"scan\":{\"attested\":%llu,\"unattested\":%llu,"
        "\"fragments\":%llu,\"malformed\":%llu,\"legacy\":%llu}",
        (unsigned long long)scan.s7_entries,
        (unsigned long long)scan.unattested,
        (unsigned long long)scan.fragments,
        (unsigned long long)scan.malformed,
        (unsigned long long)scan.legacy_entries);
    jash_writef(w, ctx,
        ",\"witness\":{\"available\":%s,\"position\":\"%s\","
        "\"scope\":\"%s\",\"entries\":%llu,\"ratchet_advances\":%llu},"
        "\"recommended_action\":%s",
        witness_available ? "true" : "false",
        !witness_available ? "unavailable" : witness_mismatch ? "mismatch" : "match",
        witness_scope,
        (unsigned long long)witness_entries,
        (unsigned long long)ratchet_advances,
        witness_mismatch ? "\"sigil quarantine apply --json\"" : "null");
    jash_writes(w, ctx, ",\"artifacts\":[");
    if (invalid_v1) {
        char escaped[PATH_MAX * 2];
        jash_json_esc(v1, escaped, sizeof escaped);
        jash_writef(w, ctx,
            "{\"path\":\"%s\",\"reason\":\"invalid_migration_v1\","
            "\"entry_count\":%llu,\"fragment_count\":%llu}",
            escaped, (unsigned long long)(old.s7_entries + old.legacy_entries),
            (unsigned long long)old.fragments);
    }
    jash_writes(w, ctx, "],\"quarantined\":[");
    quarantine_list_manifests(w, ctx);
    jash_writes(w, ctx, "],\"recovery_artifacts\":[");
    quarantine_list_recovery_artifacts(w, ctx);
    jash_writes(w, ctx, "]}\n");
    if (active) {
        (void)flock(fileno(active), LOCK_UN);
        fclose(active);
    }
    chain_transaction_unlock(transaction_fd);
    return snapshot_ok ? 0 : 1;
}

#include "sigil_recovery_observed.inc"

/* ── S3: retiring a shared ratchet lineage (rekey / adopt) ─────────────
 *
 * `legacy_global_ratchet_scope_mismatch` is a CORRECT block and must not be
 * papered over. One ~/.jash/ratchet.key advanced by six repositories cannot
 * witness the length of any one of them: its count is a SUM, so it matches
 * nobody, and "verified" would be a lie in either direction.
 *
 * There are exactly two honest ways out, and the operator chooses:
 *
 *   rekey — retire the shared lineage; start a fresh per-repo key. Strongest
 *           exclusivity going forward. The past keeps its hash chain.
 *   adopt — derive the per-repo key from the shared one, domain-separated by
 *           the repo key so the two ledgers never reuse key material.
 *           Lineage continuity is auditable, but exclusivity is WEAKER:
 *           whoever holds the shared key can derive this one. That weakness
 *           is recorded in the sidecar, not hidden.
 *
 * Both seal a boundary entry with the OUTGOING key before the new one
 * exists. That entry is the seam. Nothing is rewritten, nothing deleted, and
 * no past entry is promoted to "verified" — the retired segment keeps its
 * hash chain and loses only a position witness it never actually had. */
#include "sigil_ratchet_observed.inc"

/* ── Lifecycle ──────────────────────────────────────────────────────── */

static int sigil_init(void)
{
    sigil_pin_ledger_identity();
    if (!g_ledger.jash_dir[0]) return -1;
    chain_resume();
    g_ts_start_ns = now_ns();
    return 0;
}

/* Writer that discards output — used to run proof emission silently at exit. */
static long sigil_discard_writer(void* ctx, const char* buf, size_t n)
{ (void)ctx; (void)buf; return (long)n; }

static void cmd_proof_usage(jash_writer_t w, void* ctx)
{
    jash_writes(w, ctx,
        "sigil proof — session attestation proof\n"
        "Usage: sigil proof [emit [file] | dsse [file] | identity | verify [file] | show [file]]\n"
        "  emit     write a jashproof/1 text proof (default)\n"
        "  dsse     write a DSSE envelope\n"
        "  identity print the public key fingerprint as JSON\n"
        "  verify   verify a proof against its embedded public key and live chain\n"
        "  show     print the proof file verbatim\n");
}

static int cmd_proof_emit(jmod_writer_t w, void* ctx, const char* outpath);

static void sigil_shutdown(void)
{
    if (jash_sigil_get() >= SIGIL_LOG && !g_read_only_pending &&
        !g_last_command_read_only) {
        uint64_t uptime = now_ns() - g_ts_start_ns;
        char sentinel[128];
        snprintf(sentinel, sizeof(sentinel),
                 "SESSION_END uptime=%llums",
                 (unsigned long long)(uptime / 1000000ULL));
        chain_append(sentinel, 0);   /* fflush'd inside chain_append */
    }
    /* SIGN/SEAL: auto-emit the session proof so `JASH_SIGIL=2` actually
     * produces ~/.jash/session.jashproof at session end (the HMAC proof the
     * integration guide promises), not only when run manually. Silent. */
    if (jash_sigil_get() >= SIGIL_SIGN && !g_read_only_pending &&
        !g_last_command_read_only) {
        cmd_proof_emit(sigil_discard_writer, NULL, NULL);
    }
    if (g_chain_fp) { fclose(g_chain_fp); g_chain_fp = NULL; }
}

/* ── Hooks ──────────────────────────────────────────────────────────── */

static void sigil_native_argv_hash(int argc, char** argv, uint8_t digest[32])
{
    jash_sha256_ctx_t hash;
    jash_sha256_init(&hash);
    for (int i = 0; i < argc; i++)
        jash_sha256_update(&hash, (const uint8_t*)argv[i], strlen(argv[i]) + 1);
    jash_sha256_final(&hash, digest);
}

static void sigil_native_read_only_mark(int argc, char** argv)
{
    sigil_native_argv_hash(argc, argv, g_native_read_only_hash);
    g_native_read_only_argc = argc;
    g_read_only_pending = 1;
}

static int sigil_read_only_hook_matches(const char* command)
{
    /* Match the actual literal invocation with the shared quote-aware argv
     * tokenizer. A composite line, expansion or later command is conservatively
     * recorded; a preceding query must not make that line invisible. */
    jash_argv_t arguments = {0};
    if (!command || jash_argv_build(command, &arguments) < 0) return 0;
    int matches = 0;
    if (arguments.argc && !strcmp(arguments.argv[0], "sigil")) {
        if (g_native_read_only_argc >= 0 && arguments.argc == g_native_read_only_argc + 2 &&
            (!strcmp(arguments.argv[1], "--json") || !strcmp(arguments.argv[1], "--data") ||
             !strcmp(arguments.argv[1], "--data-v2"))) {
            uint8_t digest[32];
            sigil_native_argv_hash(arguments.argc - 2, arguments.argv + 2, digest);
            matches = !memcmp(digest, g_native_read_only_hash, sizeof digest);
        } else if (g_native_read_only_argc < 0 && arguments.argc >= 2 && arguments.argc <= 4 &&
                   !strcmp(arguments.argv[1], "quarantine")) {
            matches = arguments.argc == 2 ||
                (arguments.argc == 3 && !strcmp(arguments.argv[2], "--json")) ||
                (!strcmp(arguments.argv[2], "status") && (arguments.argc == 3 ||
                    (arguments.argc == 4 && !strcmp(arguments.argv[3], "--json"))));
        }
    }
    jash_argv_free(&arguments);
    return matches;
}

static void sigil_post_cmd(const jmod_call_t* call, int exit_code)
{
    if (jash_sigil_get() < SIGIL_LOG) {
        const char* explicit_level = getenv("JASH_SIGIL");
        if (explicit_level && atoi(explicit_level) > 0)
            fprintf(stderr, "sigil append blocked: runtime level is disabled\n");
        return;
    }
    const char* cmd = call ? call->args : NULL;
    if (g_read_only_pending) {
        /* Module-dispatched sigil verbs do not always receive post_cmd.  Skip
         * only if the hook actually names the observational command; if the
         * next hook belongs to another command, record it normally. */
        if (sigil_read_only_hook_matches(cmd)) {
            g_read_only_pending = 0;
            g_native_read_only_argc = -1;
            g_last_command_read_only = 1;
            return;
        }
        g_read_only_pending = 0;
        g_native_read_only_argc = -1;
    }
    g_last_command_read_only = 0;
    chain_append(cmd, exit_code);
}

/* ── Dispatch ───────────────────────────────────────────────────────── */

static int cmd_chain_show(jmod_writer_t w, void* ctx)
{
    FILE* f = fopen(chain_path(), "r");
    if (!f) {
        jash_writes(w, ctx, "sigil: no chain yet (run `sigil log` to start)\n");
        return 0;
    }
    char line[SIGIL_LINE_MAX];
    uint64_t shown = 0;
    while (fgets(line, sizeof(line), f) && shown < 20) {
        jash_writes(w, ctx, line);
        shown++;
    }
    if (!feof(f)) jash_writes(w, ctx, "... (truncated — inspect the active chain file)\n");
    fclose(f);
    return 0;
}


/* JINV-054 — `sigil list --json` / `sigil chain --json`: a chain como DADO.
 *
 * O `verify --json` (JINV-051/053) responde "posso confiar?". Mas quem quer
 * APOSENTAR um ledger proprio precisa da outra metade: CONSULTAR a chain.
 * Sem isso a verificacao serve para conferir, nao para substituir — foi
 * exatamente a ressalva de quem pediu, e ela estava certa.
 *
 * Cada entrada da chain e uma linha:
 *   <seq> <ts_sec>.<ts_ns> <blake3_hex> tag=<hex> rc=<rc> cmd=<cmd>
 *
 * Emitida aqui como JSONL — uma entrada por linha, nao um array unico. Um
 * ledger cresce sem teto, e um consumidor deve poder processar em fluxo sem
 * carregar tudo na memoria. (E o mesmo formato que o `journal --json` ja usa.)
 *
 * `tag` vai no dado por completude, mas veja o `not_verified` do verify: ela
 * NAO pode ser recomputada nem por este shell. Esta aqui para quem guardou a
 * chave da epoca, nao como promessa de verificabilidade.
 *
 * `limit` opcional: `sigil list --json 50` devolve as 50 mais recentes.
 * Sem limite, devolve tudo — quem substitui um ledger precisa do historico. */
static int cmd_chain_list_json(jmod_writer_t w, void* ctx, long limit)
{
    FILE* f = fopen(chain_path(), "r");
    if (!f) return 0;                      /* sem chain = zero linhas, rc 0 */

    /* Com limite, conta primeiro para saber onde comecar a emitir. */
    unsigned long total = 0;
    if (limit > 0) {
        char c;
        int last = '\n';
        while ((c = (char)fgetc(f)) != (char)EOF) { if (c == '\n') total++; last = c; }
        if (last != '\n') total++;
        rewind(f);
    }
    unsigned long pular = (limit > 0 && total > (unsigned long)limit)
                        ? total - (unsigned long)limit : 0;

    char line[SIGIL_LINE_MAX];
    unsigned long idx = 0;
    while (fgets(line, sizeof line, f)) {
        if (idx++ < pular) continue;

        uint64_t seq = 0; char ts[64] = "", hash[80] = "";
        if (sscanf(line, "%llu %63s %79s",
                   (unsigned long long*)&seq, ts, hash) < 3) continue;

        /* tag= / rc= / cmd= sao campos nomeados; cmd vai ate o fim da linha. */
        char tag[80] = ""; int rc = 0;
        const char* t = strstr(line, " tag=");
        if (t) { sscanf(t + 5, "%79s", tag); }
        const char* r = strstr(line, " rc=");
        if (r) { rc = atoi(r + 4); }
        const char* c = strstr(line, " cmd=");
        const char* cmd = c ? c + 5 : "";
        char cmdbuf[SIGIL_LINE_MAX];
        size_t n = 0;
        while (cmd[n] && cmd[n] != '\n' && n + 1 < sizeof cmdbuf) { cmdbuf[n] = cmd[n]; n++; }
        cmdbuf[n] = '\0';

        size_t ecap = 6 * strlen(cmdbuf) + 1;
        char* ecmd = (char*)malloc(ecap);
        if (!ecmd) { fclose(f); return 1; }
        jash_json_esc(cmdbuf, ecmd, ecap);
        jash_writef(w, ctx,
            "{\"seq\":%llu,\"ts\":\"%s\",\"hash\":\"%s\",\"tag\":\"%s\","
            "\"rc\":%d,\"cmd\":\"%s\"}\n",
            (unsigned long long)seq, ts, hash, tag, rc, ecmd);
        free(ecmd);
    }
    fclose(f);
    return 0;
}

/* ── S8: OpenBSD pledge — sandboxed child for chain verify ─────────── */

#ifdef __OpenBSD__
#include <unistd.h>  /* pledge() */

static void pipe_writer(void* ctx, const char* s, size_t n)
{
    int fd = *(int*)ctx;
    while (n > 0) {
        ssize_t r = write(fd, s, n);
        if (r <= 0) break;
        s += (size_t)r; n -= (size_t)r;
    }
}
#endif /* __OpenBSD__ */


/* JINV-051 — `sigil chain verify --json`.
 *
 * Um agente que precisa decidir "posso confiar neste log?" hoje le TEXTO e
 * adivinha. Mas aqui a saida de maquina tem um requisito extra, que os outros
 * verbos nao tem: ela nao pode dizer apenas `valid:true`.
 *
 * O JINV-024 encontrou o `sigil` afirmando "proof signature valid" quando so
 * havia integridade, nao autenticidade. O JINV-031 encontrou "chain intact"
 * para um log APAGADO. Nos dois casos a mensagem afirmava mais do que a
 * verificacao sustentava — e um `valid:true` cru repetiria o erro em formato
 * de maquina, que e pior, porque nenhum humano rele.
 *
 * Entao esta saida declara TRES coisas:
 *   checks       o que foi de fato verificado, item a item;
 *   not_verified o que NAO foi, e por que e impossivel;
 *   limits       o limite honesto do esquema.
 *
 * O `not_verified` nao e uma ressalva defensiva: as tags por entrada sao
 * forward-secure (k_{n+1} = BLAKE3(k_n || "r"), sentido unico), entao tags
 * antigas nao podem ser recomputadas NEM POR ESTE SHELL. Um consumidor que
 * saiba disso decide melhor; um que receba `valid:true` sem contexto decide
 * pior. */
static int cmd_chain_verify_json_locked(jmod_writer_t w, void* ctx,
                                        int held_transaction_fd)
{
    sigil_verify_observation_t r;
    int status = sigil_verify_observe(&r, held_transaction_fd);
    if (status) {
        if (r.kind == SIGIL_VERIFY_ARTIFACT)
            jash_writef(w, ctx,
                "{\"valid\":false,\"failure\":\"%s\",\"entries_verified\":0,"
                "\"entries_detected\":%llu,\"fragments\":%llu}\n", r.failure,
                (unsigned long long)r.entries_detected, (unsigned long long)r.fragments);
        else if (r.kind == SIGIL_VERIFY_STRUCTURAL)
            jash_writef(w, ctx,
                "{\"valid\":false,\"failure\":\"%s\",\"reason\":\"%s\","
                "\"entries_verified\":0,\"entries_detected\":%llu,\"fragments\":%llu}\n",
                r.failure, r.failure, (unsigned long long)r.entries_detected, (unsigned long long)r.fragments);
        else if (r.kind == SIGIL_VERIFY_WITNESS)
            jash_writef(w, ctx,
                "{\"valid\":false,\"failure\":\"%s\",\"entries_verified\":%llu,"
                "\"ratchet_advances\":%llu,\"missing\":%llu,"
                "\"detail\":\"a cadeia de hash do que sobrou esta intacta — e por "
                "isso ela sozinha nao prova nada: um prefixo truncado encadeia "
                "perfeitamente\"}\n", r.failure,
                (unsigned long long)r.entries_verified, (unsigned long long)r.ratchet_advances,
                (unsigned long long)r.missing);
        else if (r.kind == SIGIL_VERIFY_WALK)
            jash_writef(w, ctx,
                "{\"valid\":false,\"failure\":\"%s\",\"at_seq\":%llu,"
                "\"entries_verified\":%llu,\"entries_unattested\":%llu}\n",
                r.failure, (unsigned long long)r.at_sequence, (unsigned long long)r.entries_verified,
                (unsigned long long)r.entries_unattested);
        else
            jash_writef(w, ctx, "{\"valid\":false,\"failure\":\"%s\",\"entries_verified\":0}\n", r.failure);
        return status;
    }
    jash_writef(w, ctx,
        "{\"valid\":true,\"entries_verified\":%llu,\"entries_unattested\":%llu,"
        "\"sequence_restarts\":%llu,\"ratchet_advances\":%llu,"
        "\"witness_scope\":\"%s\",\"witness_entries\":%llu,\"boundaries\":%llu,"
        "\"checks\":{\"hash_chain\":\"pass\",\"attested_seq_monotonicity\":\"pass\","
        "\"ratchet_position\":\"%s\"},"
        "\"not_verified\":[\"per_entry_tags\"],"
        "\"why_not\":\"as tags sao forward-secure (k_n+1 = BLAKE3(k_n || r), "
        "sentido unico): tags antigas nao podem ser recomputadas nem por este "
        "shell\","
        "\"limits\":[\"apagar o log E a chave juntos e indistinguivel de uma "
        "instalacao nova — so uma testemunha externa (a prova assinada) cobre "
        "isso\"%s]}\n",
        (unsigned long long)r.entries_verified,
        (unsigned long long)r.entries_unattested,
        (unsigned long long)r.sequence_restarts,
        (unsigned long long)r.ratchet_advances,
        r.witness_scope, (unsigned long long)r.witness_entries,
        (unsigned long long)r.boundaries,
        "pass",
        r.witness_entries != r.entries_verified
          ? ",\"o ratchet atual testemunha apenas o segmento apos a ultima "
            "fronteira selada; as entradas anteriores dependem da prova "
            "assinada daquele segmento, nao deste ratchet\""
          : "");
    return 0;
}

static int cmd_chain_verify_json(jmod_writer_t w, void* ctx)
{
    return cmd_chain_verify_json_locked(w, ctx, -1);
}

/* range_verify: if has_range==0, verify all; else only seq in [from,to] */
static int cmd_chain_verify_range(jmod_writer_t w, void* ctx,
                                   uint64_t range_from, uint64_t range_to,
                                   int has_range)
{
    if (access(chain_path(), F_OK) != 0) {
        jash_writes(w, ctx, "sigil verify: no chain file\n");
        return 1;
    }
    int transaction_fd = -1;
    FILE* f = sigil_verify_snapshot_open(&transaction_fd);
    if (!f) {
        jash_writes(w, ctx, "sigil verify: chain/witness snapshot unavailable\n");
        return 1;
    }
    sigil_chain_scan_t fresh;
    memset(&fresh, 0, sizeof fresh);
    fresh.first_fragment_offset = -1;
    const char* recovery = sigil_scan_chain(chain_path(), &fresh) == 0
        ? sigil_scan_recovery_reason(&fresh) : "snapshot_unavailable";
    if (recovery) {
        sigil_verify_snapshot_close(f, transaction_fd);
        jash_writef(w, ctx,
            "sigil verify: KNOWN CORRUPTION — %s (entries=%llu, fragments=%llu); "
            "run `sigil quarantine status --json`\n",
            recovery, (unsigned long long)fresh.s7_entries,
            (unsigned long long)fresh.fragments);
        return 1;
    }

    char line[SIGIL_LINE_MAX];
    uint8_t running_hash[32];
    memset(running_hash, 0, sizeof(running_hash));
    /* ═══ JINV-112 / BUG-062 — a cadeia e um registro MISTO ═══════════════
     *
     * Duas coisas gravam aqui, e sempre gravaram:
     *
     *   nivel LOG (c/module/sigil.c)  hash = 64 zeros, `tag=` texto livre
     *   nivel SIGN/SEAL (este modulo) hash = BLAKE3 real, `tag=` 64 hex
     *
     * O ESCRITOR ja sabe disso: ancora `g_prev_hash` em `scan.last_s7` — a
     * ultima entrada ATESTADA — e valida com sigil_hex64(), que recusa os
     * zeros. O SCANNER tambem: JINV-103/108 classificam a linha de hash-zero
     * como `unattested` e dizem, textualmente, que ela "nao pode ancorar
     * prev_hash". So o VERIFICADOR ficou fora de sincronia: comecava em
     * expected_seq = 0 e recalculava o hash de toda linha, entao a primeira
     * entrada — do core, seq 1, hash zero — falhava nos dois criterios de uma
     * vez e `sigil chain verify` NUNCA passava, em nenhum SO.
     *
     * Medido nesta maquina: ~/.jash/chain.log tem 5562 entradas de nivel LOG
     * e 458 atestadas. Um verificador que nao sabe a diferenca reprova o
     * ledger inteiro por causa da linha 1.
     *
     * Regra, identica a do escritor e a do scanner:
     *   - LOG   conta como registro, NAO avanca running_hash, NAO prova nada
     *   - ATEST encadeia o hash e conta para a testemunha do ratchet
     *
     * `checked` conta so as atestadas de proposito: e ele que a checagem do
     * ratchet (JINV-031) compara com g_ratchet_advances, e o ratchet so avanca
     * quando ESTE modulo grava. Contar LOG ali produziria "ENTRADAS FORJADAS"
     * num ledger intacto.
     *
     * F E R M I ∞ H A R T */
    uint64_t expected_seq = 0, checked = 0, unattested = 0, segments = 0;
    int seq_base_set = 0;
    uint64_t prev_ts_ns = 0;
    int ok = 1, liveness_warns = 0;
    sigil_boundary_t bnd = {0};

    while (fgets(line, sizeof(line), f)) {
        uint64_t seq = 0; char ts_str[32]; char hexhash[65]; int rc = 0;
        if (!strchr(line, '\n')) {
            jash_writef(w, ctx, "sigil verify: malformed physical line near entry %llu\n",
                        (unsigned long long)(checked + 1));
            ok = 0;
            break;
        }
        if (sscanf(line, "%llu %31s %64s",
                   (unsigned long long*)&seq, ts_str, hexhash) < 3) {
            if (!has_range || seq >= range_from) {
                jash_writef(w, ctx, "sigil verify: malformed line near seq %llu\n",
                            (unsigned long long)(checked+1));
                ok = 0; break;
            }
            continue;
        }
        /* Classify fallback LOG before consulting any attested state. Its seq
         * may come from the core's independent counter and legitimately roll
         * back between two S7 records. It also cannot introduce a boundary. */
        if (sigil_line_is_unattested_log(line)) {
            if (!has_range || (seq >= range_from && seq <= range_to)) unattested++;
            continue;
        }

        const char* tag_ptr = strstr(line, " tag=");
        const char* rc_ptr = strstr(line, " rc=");
        const char* cmd_ptr = strstr(line, " cmd=");
        char taghex[65] = {0};
        int after_tag = 0;
        if (!sigil_hex64(hexhash) || !sigil_fallback_timestamp(ts_str) ||
            !tag_ptr || sscanf(tag_ptr, " tag=%64s%n", taghex, &after_tag) != 1 ||
            !sigil_hex64(taghex) ||
            !(tag_ptr[after_tag] == ' ' || tag_ptr[after_tag] == '\t') ||
            !rc_ptr || sscanf(rc_ptr, " rc=%d", &rc) < 1 || !cmd_ptr) {
            ok = 0; break;
        }
        /* A base do seq vem da PRIMEIRA linha, nunca de um 0 fixo. O contador
         * (~/.jash/chain.seq) e GLOBAL enquanto as cadeias sao por-repo, entao
         * uma cadeia criada com o contador em 400 comeca legitimamente em 401.
         * Medido: as cadeias por-repo comecam em 0 ou 1 e sao contiguas; a
         * chain.log historica tem 6020 linhas com seq maximo 459 — numeros
         * REPETIDOS, porque repos diferentes consumiram o mesmo contador.
         *
         * Por isso a contiguidade de seq nao e o detector de truncamento aqui;
         * quem faz esse trabalho e o ratchet (JINV-031, abaixo). O que ainda
         * vale exigir e que o seq nunca ANDE PARA TRAS: isso pega reordenacao
         * e insercao no meio, que e o que um seq monotonico pode honestamente
         * testemunhar. */
        if (!seq_base_set) { expected_seq = seq; seq_base_set = 1; }
        if (seq < expected_seq) {
            /* Retroceder PARA O INICIO (0 ou 1) e reinicio de numeracao, nao
             * adulteracao. Antes do ADR-0051 W3 o contador era estatico por
             * PROCESSO: cada `jash -c` recomecava em 1, e a cadeia acumulou um
             * segmento por invocacao. Medido em ~/.jash/chain.log: 972
             * retrocessos, 972 deles para 0 ou 1 e NENHUM para um valor
             * intermediario. Essa e a assinatura de reinicio.
             *
             * Um retrocesso para o MEIO e outra coisa — e o que reordenacao ou
             * insercao produziria — e continua reprovando. A distincao vem do
             * dado, nao de conveniencia: reprovar todo ledger legado tornaria o
             * verificador inutil justamente para quem tem historico, e aceitar
             * qualquer retrocesso jogaria fora a unica ordem que o seq
             * testemunha. */
            if (seq <= 1) {
                segments++;
            } else if (!has_range || seq >= range_from) {
                jash_writef(w, ctx,
                    "sigil verify: \033[1;31mseq RETROCEDEU\033[0m em %llu "
                    "(esperado >= %llu)\n"
                    "  Um retrocesso para o meio da cadeia nao e reinicio de\n"
                    "  numeracao: e reordenacao ou insercao.\n",
                    (unsigned long long)seq, (unsigned long long)expected_seq);
                ok = 0; break;
            }
        }

        uint64_t ts_sec = 0, ts_nsec = 0;
        sscanf(ts_str, "%llu.%llu",
               (unsigned long long*)&ts_sec, (unsigned long long*)&ts_nsec);
        uint64_t ts_ns = ts_sec * 1000000000ULL + ts_nsec;

        if (prev_ts_ns > 0 && ts_ns + 2000000000ULL < prev_ts_ns) {
            jash_writef(w, ctx,
                "\033[1;33m⚠ LIVENESS WARN\033[0m seq %llu — timestamp rollback "
                "(delta -%llums)\n",
                (unsigned long long)seq,
                (unsigned long long)((prev_ts_ns - ts_ns) / 1000000ULL));
            liveness_warns++;
        }
        prev_ts_ns = ts_ns;

        const char* cmd = cmd_ptr + 5;
        size_t clen = strlen(cmd);
        if (clen > 0 && cmd[clen-1] == '\n') clen--;

        uint8_t payload[4096]; size_t plen = 0;
        for (int i=7;i>=0;i--) { payload[plen++]=(uint8_t)(ts_ns>>(i*8)); }
        payload[plen++]=(uint8_t)(rc&0xFF); payload[plen++]=(uint8_t)((rc>>8)&0xFF);
        payload[plen++]=(uint8_t)((rc>>16)&0xFF); payload[plen++]=(uint8_t)((rc>>24)&0xFF);
        if (clen>SIGIL_PAYLOAD_CMD_MAX(plen)) clen=SIGIL_PAYLOAD_CMD_MAX(plen);
        if (clen) { memcpy(payload+plen,cmd,clen); plen+=clen; }

        uint8_t expected_hash[32];
        jash_blake3_chain(running_hash, payload, plen, expected_hash);
        memcpy(running_hash, expected_hash, 32);

        if (!has_range || (seq >= range_from && seq <= range_to)) {
            char expected_hex[JASH_BLAKE3_HEX_LEN];
            jash_blake3_hex(expected_hash, expected_hex);
            if (strcmp(expected_hex, hexhash) != 0) {
                jash_writef(w, ctx,
                    "sigil verify: \033[1;31mCHAIN BROKEN\033[0m at seq %llu\n"
                    "  expected: %s\n  got:      %s\n",
                    (unsigned long long)seq, expected_hex, hexhash);
                ok = 0; break;
            }
            checked++;
        }
        sigil_boundary_track(&bnd, cmd);

        expected_seq = seq + 1;
        if (has_range && seq >= range_to) break;
    }
    if (ferror(f)) ok = 0;
    sigil_verify_snapshot_close(f, transaction_fd);

    /* JINV-031 — a cadeia de hash sozinha NAO detecta truncamento.
     *
     * Remover as ultimas entradas deixa um prefixo perfeitamente valido: todos
     * os hashes encadeiam, e a checagem de lacuna de `seq` nao ve nada porque
     * nao ha lacuna — o fim simplesmente nao existe mais. Medido antes desta
     * correcao:
     *
     *   apagar as 3 ultimas entradas  ->  "✓ chain intact — 4 entries verified"
     *   apagar o log INTEIRO          ->  "✓ chain intact — 0 entries verified"
     *
     * Um checkmark verde para "o log de auditoria sumiu". Numa ferramenta cuja
     * tese e atestacao, esse e o unico veredito que nao pode existir.
     *
     * O `tag=` (MAC de ratchet) era escrito em toda entrada e NUNCA lido — e
     * como ele e forward-secure, tags antigas nao podem ser recomputadas nem
     * pelo proprio shell. Mas a POSICAO do ratchet pode: ele avanca uma vez por
     * entrada, e a chave nova nao pode ser rebobinada para a anterior (a
     * derivacao e um sentido so). Entao a contagem de avancos e uma testemunha
     * INDEPENDENTE de quantas entradas existiram — e um atacante que apaga o
     * fim do log nao consegue desfazer os avancos que ja aconteceram.
     *
     * Limite honesto, declarado em vez de escondido: apagar o log E a chave
     * juntos e indistinguivel de uma instalacao nova. Nenhum esquema local
     * resolve isso — so uma testemunha externa (a prova assinada, JINV-024). */
    if (ok && !has_range) {
        /* JINV-108: judge the segment the ratchet actually witnesses. Before
         * this, an honest `sigil ratchet rekey` made this branch scream
         * ENTRADAS FORJADAS at the operator who had just rotated his own key. */
        const char* wscope = "whole_chain";
        unsigned long long entradas =
            (unsigned long long)sigil_witness_entries(&bnd, checked, &wscope);
        if (g_ratchet_advances > entradas) {
            jash_writef(w, ctx,
                "sigil verify: \033[1;31mLOG TRUNCADO\033[0m — o ratchet avancou "
                "%llu vezes, mas a cadeia tem %llu entradas.\n"
                "  Faltam %llu entrada(s). A cadeia de hash do que sobrou esta\n"
                "  intacta, e e exatamente por isso que ela sozinha nao prova nada:\n"
                "  um prefixo truncado encadeia perfeitamente.\n",
                (unsigned long long)g_ratchet_advances, entradas,
                (unsigned long long)(g_ratchet_advances - entradas));
            ok = 0;
        } else if (entradas > g_ratchet_advances) {
            jash_writef(w, ctx,
                "sigil verify: \033[1;31mENTRADAS FORJADAS\033[0m — a cadeia tem "
                "%llu entradas, mas o ratchet so avancou %llu vezes.\n"
                "  %llu entrada(s) foram acrescentadas sem passar pelo ratchet.\n",
                entradas, (unsigned long long)g_ratchet_advances,
                (unsigned long long)(entradas - g_ratchet_advances));
            ok = 0;
        }
    }

    if (ok) {
        if (has_range)
            jash_writef(w, ctx,
                "\033[1;32m✓ chain intact\033[0m — %llu entries verified (seq %llu..%llu)",
                (unsigned long long)checked,
                (unsigned long long)range_from, (unsigned long long)range_to);
        else
            jash_writef(w, ctx,
                "\033[1;32m✓ chain intact\033[0m — %llu entries verified "
                "(hash encadeado + posicao do ratchet)",
                (unsigned long long)checked);
        /* Diz as duas coisas separadamente. As entradas de nivel LOG existem
         * no ledger e foram contadas, mas nao provam nada — e um verde que as
         * misturasse com as atestadas estaria mentindo sobre o tamanho da
         * prova. Mesmo tom do `sigil ratchet rekey`: "o segmento aposentado
         * NAO e promovido a verificado ... o que e a verdade". */
        if (unattested)
            jash_writef(w, ctx, " · \033[90m%llu registro(s) de nivel LOG "
                        "contados, NAO atestados\033[0m",
                        (unsigned long long)unattested);
        if (segments)
            jash_writef(w, ctx, " · \033[90m%llu reinicio(s) de numeracao "
                        "(historico pre-ADR-0051)\033[0m",
                        (unsigned long long)segments);
        if (liveness_warns)
            jash_writef(w, ctx, ", \033[1;33m%d liveness warn(s)\033[0m\n", liveness_warns);
        else
            jash_writes(w, ctx, "\n");
    }
    return ok ? 0 : 1;
}

static int cmd_chain_verify(jmod_writer_t w, void* ctx)
{
#ifdef __OpenBSD__
    /* S8: fork a child, pledge it to stdio+rpath only, relay output via pipe */
    int pfd[2];
    if (pipe(pfd) == 0) {
        pid_t pid = fork();
        if (pid == 0) {
            close(pfd[0]);
            pledge("stdio rpath", NULL);
            int fd = pfd[1];
            /* SECURITY: propagate the verdict — the old code discarded the
             * return value and always _exit(0), so every automated consumer
             * (CI, `expect rc 0`) saw PASS on a broken chain. */
            int vrc = cmd_chain_verify_range(pipe_writer, &fd, 0, 0, 0);
            close(pfd[1]);
            _exit(vrc == 0 ? 0 : 1);
        }
        if (pid > 0) {
            close(pfd[1]);
            char buf[512]; ssize_t n;
            while ((n = read(pfd[0], buf, sizeof(buf))) > 0)
                w(ctx, buf, (size_t)n);
            close(pfd[0]);
            int st = 0; waitpid(pid, &st, 0);
            return (WIFEXITED(st) && WEXITSTATUS(st) == 0) ? 0 : 1;
        }
        close(pfd[0]); close(pfd[1]);
    }
#endif
    return cmd_chain_verify_range(w, ctx, 0, 0, 0);
}

/* ── S2: .jashproof emit + verify ──────────────────────────────────── */

static uint64_t chain_count_entries(void) {
    FILE* f = fopen(chain_path(), "r");
    if (!f) return 0;
    uint64_t n = 0;
    char line[SIGIL_LINE_MAX];
    while (fgets(line, sizeof(line), f)) {
        const char* cp = strstr(line, " cmd=");
        if (cp && strncmp(cp + 5, "SESSION_END", 11) != 0) n++;
    }
    fclose(f);
    return n;
}

typedef struct {
    uint64_t entries;
    uint8_t tip[32];
} sigil_proof_snapshot_t;

static int proof_snapshot_capture(sigil_proof_snapshot_t* snapshot)
{
    memset(snapshot, 0, sizeof *snapshot);
    int transaction_fd = chain_transaction_lock();
    if (transaction_fd < 0) return -1;
    FILE* chain = NULL;
    for (int attempt = 0; attempt < 3; attempt++) {
        chain = fopen(chain_path(), "r");
        if (!chain) break;
        if (flock(fileno(chain), LOCK_SH) != 0) { fclose(chain); chain = NULL; break; }
        if (chain_stream_is_active(chain)) break;
        (void)flock(fileno(chain), LOCK_UN);
        fclose(chain);
        chain = NULL;
    }
    if (!chain) {
        chain_transaction_unlock(transaction_fd);
        return -1;
    }

    char line[SIGIL_LINE_MAX];
    while (fgets(line, sizeof line, chain)) {
        const char* cmd = strstr(line, " cmd=");
        if (cmd && strncmp(cmd + 5, "SESSION_END", 11) != 0)
            snapshot->entries++;
        if (sigil_line_can_anchor(line)) {
            char ts[32], hex[65];
            unsigned long long seq = 0;
            if (sscanf(line, "%llu %31s %64s", &seq, ts, hex) == 3)
                (void)hexdec(hex, snapshot->tip, sizeof snapshot->tip);
        }
    }
    int ok = ferror(chain) ? -1 : 0;
    (void)flock(fileno(chain), LOCK_UN);
    fclose(chain);
    chain_transaction_unlock(transaction_fd);
    return ok;
}

typedef struct { int published, durable, error_number; } sigil_publication_t;

static int proof_publish_observed(const char* path, const char* data, size_t len,
                                   sigil_publication_t* observation)
{
    memset(observation, 0, sizeof *observation);
    char tmp[PATH_MAX];
    int n = snprintf(tmp, sizeof tmp, "%s.tmp.%ld.%llu", path, (long)getpid(),
                     (unsigned long long)now_ns());
    if (n < 0 || (size_t)n >= sizeof tmp) { observation->error_number = ENAMETOOLONG; return -1; }
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) { observation->error_number = errno; return -1; }
    FILE* out = fdopen(fd, "w");
    if (!out) { observation->error_number = errno; close(fd); unlink(tmp); return -1; }
    const char* fault = getenv("JASH_SIGIL_TEST_PROOF_FAIL");
    int ok = !(fault && strcmp(fault, "write") == 0) &&
             fwrite(data, 1, len, out) == len;
    if (ok && fflush(out) != 0) ok = 0;
    if (ok && fsync(fd) != 0) ok = 0;
    if (fault && strcmp(fault, "fsync") == 0) ok = 0;
    int close_ok = fclose(out) == 0;
    if (fault && strcmp(fault, "close") == 0) close_ok = 0;
    if (!ok || !close_ok || (fault && strcmp(fault, "rename") == 0) ||
        rename(tmp, path) != 0) {
        observation->error_number = errno ? errno : EIO;
        unlink(tmp);
        return -1;
    }
    observation->published = 1;
    int durable = sigil_fsync_parent(path) == 0;
    if (fault && strcmp(fault, "dir-fsync") == 0) durable = 0;
    observation->durable = durable;
    if (!durable) observation->error_number = errno ? errno : EIO;
    return durable ? 0 : -1;
}

static int proof_publish_atomic(const char* path, const char* data, size_t len)
{
    sigil_publication_t observation;
    return proof_publish_observed(path, data, len, &observation);
}

static int build_proof_body(char* buf, size_t cap,
                              uint64_t start_ts, uint64_t end_ts,
                              uint64_t entries, const uint8_t tip[32])
{
    char tip_hex[JASH_BLAKE3_HEX_LEN];
    jash_blake3_hex(tip, tip_hex);

    char host[64] = "unknown";
    gethostname(host, sizeof(host));

    const char* user = getenv("USER");
    if (!user || !*user) user = getenv("LOGNAME");
    if (!user || !*user) user = "unknown";

    char sid_hex[17];
    {
        uint8_t sid[32];
        jash_blake3((const uint8_t*)&start_ts, 8, sid);
        for (int i=0; i<8; i++)
            snprintf(sid_hex+i*2, 3, "%02x", (unsigned)sid[i]);
        sid_hex[16] = '\0';
    }

    /* W4b: self-describing Ed25519 public key so any third party can verify
     * the proof with only the public key (no shared secret). */
    char pk_hex[2*ED25519_PUBKEY + 1] = "unavailable";
    if (ed_key_load() == 0) hexenc(g_ed_pk, ED25519_PUBKEY, pk_hex);

    return snprintf(buf, cap,
        "jashproof/1\n"
        "session_id: %s\n"
        "start_ts: %llu.%09llu\n"
        "end_ts: %llu.%09llu\n"
        "user: %s\n"
        "host: %s\n"
        "chain_file: %s\n"
        "chain_entries: %llu\n"
        "chain_tip: %s\n"
        "pubkey: %s\n"
        "sig_algo: ed25519\n",
        sid_hex,
        (unsigned long long)(start_ts / 1000000000ULL),
        (unsigned long long)(start_ts % 1000000000ULL),
        (unsigned long long)(end_ts / 1000000000ULL),
        (unsigned long long)(end_ts % 1000000000ULL),
        user, host,
        chain_path(),
        (unsigned long long)entries,
        tip_hex, pk_hex);
}

static int cmd_proof_emit(jmod_writer_t w, void* ctx, const char* outpath)
{
    if (ed_key_load() != 0) {
        jash_writes(w, ctx, "sigil proof: could not load Ed25519 signing key\n");
        return 1;
    }

    sigil_proof_snapshot_t snapshot;
    if (proof_snapshot_capture(&snapshot) != 0) {
        jash_writes(w, ctx, "sigil proof: could not capture active chain snapshot\n");
        return 1;
    }
    uint64_t end_ts = now_ns();

    char body[2048];
    int blen = build_proof_body(body, sizeof(body), g_ts_start_ns, end_ts,
                                snapshot.entries, snapshot.tip);
    /* SECURITY: snprintf returns the WOULD-BE length when truncated, so blen
     * can exceed sizeof(body) while body holds only a truncated copy — the
     * old `blen < 0` check then had ed25519_sign read past the stack buffer
     * and publish a signature of adjacent stack memory. */
    if (blen < 0 || blen >= (int)sizeof(body)) {
        jash_writes(w, ctx, "sigil proof: body too large\n");
        return 1;
    }

    /* W4b: sovereign asymmetric signature — verifiable by anyone holding the
     * public key (which the proof embeds), no shared secret required. */
    /* JINV-014: ed25519_sign devolve -1 SEM tocar em sig[] quando a mensagem
     * passa do teto. Ignorar o retorno gravava 64 bytes de lixo de pilha como
     * "assinatura" e dizia ao usuario que a prova foi emitida. Falhar ALTO. */
    uint8_t sig[ED25519_SIG];
    if (ed25519_sign(sig, (const uint8_t*)body, (uint32_t)blen, g_ed_sk) != 0) {
        jash_writes(w, ctx, "sigil proof: FALHA ao assinar (corpo grande demais) — nada emitido\n");
        return 1;
    }
    char sig_hex[2*ED25519_SIG + 1];
    hexenc(sig, ED25519_SIG, sig_hex);

    if (!outpath || !*outpath) outpath = sigil_identity_proof();
    char artifact[2304];
    int artifact_len = snprintf(artifact, sizeof artifact, "%ssig: %s\n", body, sig_hex);
    if (artifact_len < 0 || (size_t)artifact_len >= sizeof artifact ||
        proof_publish_atomic(outpath, artifact, (size_t)artifact_len) != 0) {
        jash_writef(w, ctx, "sigil proof: durable atomic publish failed: %s\n", outpath);
        return 1;
    }

    char tip_hex[JASH_BLAKE3_HEX_LEN];
    jash_blake3_hex(snapshot.tip, tip_hex);
    char tip_short[17] = {0}; memcpy(tip_short, tip_hex, 16);
    char pk_hex[2*ED25519_PUBKEY + 1]; hexenc(g_ed_pk, ED25519_PUBKEY, pk_hex);
    char pk_short[17] = {0}; memcpy(pk_short, pk_hex, 16);
    jash_writef(w, ctx, "proof written: %s\n"
                "chain_tip: %s...\nsig_algo: ed25519  pubkey: %s...\n",
                outpath, tip_short, pk_short);
    return 0;
}

static int cmd_proof_identity(jmod_writer_t w, void* ctx)
{
    if (ed_key_load() != 0) {
        jash_writes(w, ctx, "{\"available\":false,\"algorithm\":\"ed25519\"}\n");
        return 1;
    }
    char keyid[2 * ED25519_PUBKEY + 1];
    hexenc(g_ed_pk, ED25519_PUBKEY, keyid);
    jash_writef(w, ctx,
        "{\"available\":true,\"algorithm\":\"ed25519\","
        "\"key_fingerprint\":\"%s\"}\n", keyid);
    return 0;
}

/* W4-intoto: emit the proof wrapped in a DSSE (Dead-Simple Signing Envelope)
 * — the in-toto/sigstore standard. The Ed25519 signature is over the DSSE PAE
 * (not the raw body), so standard DSSE verifiers accept it. */
static int cmd_proof_dsse(jmod_writer_t w, void* ctx, const char* outpath)
{
    if (ed_key_load() != 0) {
        jash_writes(w, ctx, "sigil proof dsse: could not load Ed25519 key\n");
        return 1;
    }
    sigil_proof_snapshot_t snapshot;
    if (proof_snapshot_capture(&snapshot) != 0) {
        jash_writes(w, ctx, "sigil proof dsse: could not capture active chain snapshot\n");
        return 1;
    }
    char body[2048];
    int blen = build_proof_body(body, sizeof(body), g_ts_start_ns, now_ns(),
                                snapshot.entries, snapshot.tip);
    /* SECURITY: same truncated-snprintf bound as cmd_proof_emit — blen can be
     * the would-be length (> sizeof(body)) with only a truncated copy in body. */
    if (blen < 0 || blen >= (int)sizeof(body)) {
        jash_writes(w, ctx, "sigil proof dsse: body too large\n");
        return 1;
    }

    uint8_t pae[2560];
    int paelen = dsse_pae(DSSE_PAYLOAD_TYPE, body, (size_t)blen, pae, sizeof(pae));
    if (paelen < 0) { jash_writes(w, ctx, "sigil proof dsse: PAE too large\n"); return 1; }

    /* JINV-014: idem no caminho DSSE. */
    uint8_t sig[ED25519_SIG];
    if (ed25519_sign(sig, pae, (uint32_t)paelen, g_ed_sk) != 0) {
        jash_writes(w, ctx, "sigil proof dsse: FALHA ao assinar (PAE grande demais) — nada emitido\n");
        return 1;
    }

    char payload_b64[4096]; b64enc((const uint8_t*)body, (size_t)blen, payload_b64);
    char sig_b64[128];      b64enc(sig, ED25519_SIG, sig_b64);
    char keyid[2*ED25519_PUBKEY + 1]; hexenc(g_ed_pk, ED25519_PUBKEY, keyid);

    if (!outpath || !*outpath) outpath = sigil_identity_proof_dsse();
    char envelope[6144];
    int envelope_len = snprintf(envelope, sizeof envelope,
        "{\n"
        "  \"payloadType\": \"%s\",\n"
        "  \"payload\": \"%s\",\n"
        "  \"signatures\": [\n"
        "    { \"keyid\": \"%s\", \"sig\": \"%s\" }\n"
        "  ]\n"
        "}\n",
        DSSE_PAYLOAD_TYPE, payload_b64, keyid, sig_b64);
    if (envelope_len < 0 || (size_t)envelope_len >= sizeof envelope ||
        proof_publish_atomic(outpath, envelope, (size_t)envelope_len) != 0) {
        jash_writef(w, ctx, "sigil proof dsse: durable atomic publish failed: %s\n", outpath);
        return 1;
    }
    jash_writef(w, ctx, "DSSE envelope written: %s\n  payloadType: %s\n  keyid: %.16s...\n",
                outpath, DSSE_PAYLOAD_TYPE, keyid);
    return 0;
}

/* (DSSE verification moved to libjef — jef_verify_proof. The module delegates
 * via cmd_proof_verify, so there is a single verifier implementation.) */


/* JINV-024 — dizer o que a verificacao SUSTENTA, nao mais do que isso.
 *
 * `jef_verify_proof` valida a assinatura com a chave publica que vem DENTRO da
 * prova. Isso demonstra que ela nao foi adulterada depois de assinada — e so.
 * NAO demonstra QUEM assinou: qualquer um gera um par novo, assina uma prova
 * forjada, embute a propria chave publica, e recebia o mesmo "✓ proof
 * signature valid" que uma prova legitima.
 *
 * Numa ferramenta cuja tese e atestacao, essa e a falha mais cara possivel —
 * nao porque a cripto esteja errada, mas porque a MENSAGEM afirmava origem e a
 * verificacao so sustentava integridade. E o diagnostico central da auditoria
 * (diferenciais na camada de UX, nao na de imposicao) na sua forma mais pura.
 *
 * Agora a saida distingue os dois casos. A ancora e a identidade persistente
 * do proprio dispositivo (~/.jash/keys/ed25519.seed), mais o que estiver em
 * ~/.jash/keys/trusted.pub (uma chave hex por linha).
 * Gate: tests/test_jinv024_proof_origin.sh */
static int sigil_pubkey_is_trusted(const char* hex) {
    if (!hex) return 0;
    /* 1. a identidade deste dispositivo */
    char mine[2*ED25519_PUBKEY + 1];
    if (ed_key_load() == 0) {
        hexenc(g_ed_pk, ED25519_PUBKEY, mine);
        int same = 1;
        for (int i = 0; i < 64; i++) {
            char a = hex[i], b = mine[i];
            if (a >= 'A' && a <= 'F') a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'F') b = (char)(b - 'A' + 'a');
            if (a != b) { same = 0; break; }
        }
        if (same) return 1;
    }
    /* 2. chaves confiadas explicitamente */
    const char* path = sigil_identity_trusted_pub();
    FILE* tf = fopen(path, "r");
    if (!tf) return 0;
    char line[256];
    int hit = 0;
    while (!hit && fgets(line, sizeof line, tf)) {
        int i = 0;
        while (i < 64 && line[i] && hex[i]) {
            char a = line[i], b = hex[i];
            if (a >= 'A' && a <= 'F') a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'F') b = (char)(b - 'A' + 'a');
            if (a != b) break;
            i++;
        }
        if (i == 64) hit = 1;
    }
    fclose(tf);
    return hit;
}

/* Emite o veredito honesto para uma prova cuja assinatura ja fechou. */
static void sigil_report_origin(jash_writer_t w, void* ctx, const char* inpath) {
    char pk[65];
    if (jef_proof_pubkey_hex(inpath, pk, sizeof pk) != 0) {
        jash_writes(w, ctx,
            "\033[1;32m✓ assinatura integra\033[0m — mas nao consegui ler a "
            "chave publica embutida: origem NAO verificada\n");
        return;
    }
    if (sigil_pubkey_is_trusted(pk)) {
        jash_writef(w, ctx,
            "\033[1;32m✓ assinatura valida e de identidade CONHECIDA\033[0m "
            "(%.16s…)\n", pk);
    } else {
        jash_writef(w, ctx,
            "\033[1;33m✓ assinatura integra, origem DESCONHECIDA\033[0m — "
            "assinada por %.16s…, que nao e a identidade deste dispositivo nem "
            "consta em ~/.jash/keys/trusted.pub.\n"
            "  Integridade != autenticidade: qualquer um pode assinar uma prova "
            "forjada com a propria chave. Adicione a chave em trusted.pub se "
            "voce a reconhece.\n", pk);
    }
}

/* JINV-051/053: a proof is valid for the chain state at emission time.
 * If the live tip changed, the proof no longer attests the current state.
 * Distinguish "chain grew past the proof tip" (obsolete, re-emit) from
 * "chain diverged from the proof tip" (tampering) so the user sees WHY.
 * `proof_entries` is the chain_entries value recorded in the proof body.
 * We verify the live chain through libjef rather than trusting the in-memory
 * tip, so truncation/adulteration that keeps the same tip+count is still caught. */
static int chain_tip_status(const char* proof_tip, char* current_tip,
                            uint64_t proof_entries)
{
    /* INV-PERF10-SIGIL-PROOF-001: proof counts exclude closing sentinels,
     * but its tip includes them. Verify count and tip from one locked stream,
     * never compare the total record count to the proof's command count or
     * use a process-local tip that another shell may already have advanced. */
    int transaction_fd = -1;
    FILE* chain = sigil_verify_snapshot_open(&transaction_fd);
    if (!chain) return 2;
    jef_chain_state_t state;
    int chain_ok = jef_verify_chain_stream(chain, &state) == 0 &&
                   chain_stream_is_active(chain);
    sigil_verify_snapshot_close(chain, transaction_fd);
    if (!chain_ok) return 2;
    jash_blake3_hex(state.tip, current_tip);
    if (state.commands == proof_entries && strcmp(proof_tip, current_tip) == 0)
        return 0; /* matches */
    if (state.commands > proof_entries)
        return 3; /* chain grew past the proof — obsolete */
    return 2;       /* diverged or tampered */
}

static uint64_t proof_parse_chain_entries(const char* peek)
{
    const char* p = strstr(peek, "chain_entries: ");
    if (!p) return 0;
    p += 15;
    return (uint64_t)strtoull(p, NULL, 10);
}

static int cmd_proof_verify(jmod_writer_t w, void* ctx, const char* inpath)
{
    /* JINV-024+: anchored audit: `sigil proof verify --pubkey <hex> [file]`
     * verifies that the proof was signed by the EXPECTED key, not just by
     * *some* key. Without this, the verifier is self-referential. */
    const char* pinned_hex = NULL;
    char pinned_buf[65];
    const char* path_arg = inpath;
    if (path_arg && *path_arg) {
        const char* p = path_arg;
        while (*p == ' ') p++;
        if (strncmp(p, "--pubkey", 8) == 0 && (p[8] == ' ' || p[8] == '\0')) {
            p += 8;
            while (*p == ' ') p++;
            int i = 0;
            while (i < 64 && p[i] && p[i] != ' ' &&
                   ((p[i] >= '0' && p[i] <= '9') ||
                    (p[i] >= 'a' && p[i] <= 'f') ||
                    (p[i] >= 'A' && p[i] <= 'F'))) {
                pinned_buf[i] = p[i];
                i++;
            }
            if (i != 64 || (p[i] && p[i] != ' ')) {
                jash_writes(w, ctx,
                    "sigil proof verify: --pubkey requires a 64-char hex Ed25519 public key\n");
                return 2;
            }
            pinned_buf[64] = '\0';
            p += i;
            while (*p == ' ') p++;
            pinned_hex = pinned_buf;
            path_arg = *p ? p : NULL;
        }
    }

    if (!path_arg || !*path_arg) {
        path_arg = sigil_identity_proof();
    }

    FILE* f = fopen(path_arg, "r");
    if (!f) {
        jash_writef(w, ctx, "sigil proof verify: cannot open %s\n", path_arg);
        return 1;
    }

    /* Peek to classify: DSSE envelope (leading '{') or text ed25519 proof both
     * delegate to the libjef verifier (jef_verify_proof) — single source of
     * truth for the sovereign crypto. Only legacy blake3-keyed MACs (which need
     * the session secret) stay in-module. (ROADMAP W3b) */
    char peek[8192];
    size_t pn = fread(peek, 1, sizeof(peek) - 1, f);
    peek[pn] = '\0';
    fclose(f);
    const char* s = peek;
    while (*s==' '||*s=='\n'||*s=='\t'||*s=='\r') s++;
    int is_dsse    = (*s == '{');
    int is_ed25519 = !is_dsse && (strstr(peek, "sig_algo: ed25519") != NULL);

    if (is_dsse || is_ed25519) {
        int base_ok = pinned_hex
            ? (jef_verify_proof_pinned(path_arg, pinned_hex) == 0)
            : (jef_verify_proof(path_arg) == 0);
        if (!base_ok) {
            jash_writes(w, ctx, "\033[1;31m✗ proof signature INVALID\033[0m\n");
            return 1;
        }
        sigil_report_origin(w, ctx, path_arg);        /* JINV-024 */
        /* chain_tip freshness note (text proofs only — DSSE body is base64). */
        if (is_ed25519) {
            const char* tip_ptr = strstr(peek, "chain_tip: ");
            if (tip_ptr) {
                char proof_tip[65] = {0};
                strncpy(proof_tip, tip_ptr + 11, 64);
                char current_tip[JASH_BLAKE3_HEX_LEN];
                int tip_status = chain_tip_status(proof_tip, current_tip,
                                                  proof_parse_chain_entries(peek));
                if (tip_status == 0)
                    jash_writes(w, ctx, "\033[1;32m✓ chain_tip matches live chain\033[0m\n");
                else if (tip_status == 3) {
                    jash_writes(w, ctx,
                        "\033[1;33m✗ chain_tip in proof is obsolete\033[0m\n"
                        "  the chain grew past the proof; re-emit the proof to cover it\n");
                    return 3;
                } else {
                    jash_writes(w, ctx,
                        "\033[1;31m✗ chain_tip in proof differs from live chain\033[0m\n"
                        "  possible tampering or a different chain\n");
                    return 2;
                }
            }
        }
        return 0;
    }

    if (pinned_hex) {
        jash_writes(w, ctx,
            "sigil proof verify: --pubkey is only supported for ed25519/DSSE proofs\n");
        return 1;
    }

    /* Legacy symmetric MAC path (blake3-keyed / hmac-sha256) — needs the key. */
    f = fopen(path_arg, "r");
    if (!f) { jash_writef(w, ctx, "sigil proof verify: cannot open %s\n", path_arg); return 1; }

    char body[2048] = {0};
    char stored_sig[2*ED25519_SIG + 1] = {0};
    char line[256];
    size_t body_len = 0;

    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "sig: ", 5) == 0) {
            size_t siglen = strcspn(line + 5, "\r\n");
            if (siglen >= sizeof(stored_sig)) { fclose(f); return 1; }
            memcpy(stored_sig, line + 5, siglen);
            stored_sig[siglen] = '\0';
            break;
        }
        size_t ll = strlen(line);
        if (body_len + ll < sizeof(body)) {
            memcpy(body + body_len, line, ll);
            body_len += ll;
        }
    }
    fclose(f);

    if (!stored_sig[0]) {
        jash_writes(w, ctx, "sigil proof verify: no sig: line found\n");
        return 1;
    }

    int sig_ok = 0;
    {
        uint8_t key[32];
        if (jash_session_key_load(key) != 0) {
            jash_writes(w, ctx, "sigil proof verify: could not load session key\n");
            return 1;
        }
        uint8_t mac[32]; char mac_hex[JASH_BLAKE3_HEX_LEN];
        if (strstr(body, "sig_algo: blake3-keyed") != NULL) {
            jash_blake3_keyed(key, (const uint8_t*)body, body_len, mac);
            jash_blake3_hex(mac, mac_hex);
        } else {
            jash_hmac_sha256(key, 32, (const uint8_t*)body, body_len, mac);
            jash_sha256_hex(mac, mac_hex);
        }
        sig_ok = (strcmp(mac_hex, stored_sig) == 0);
    }

    if (sig_ok) {
        jash_writes(w, ctx, "\033[1;32m✓ proof signature valid\033[0m\n");
        const char* tip_ptr = strstr(body, "chain_tip: ");
        if (tip_ptr) {
            char proof_tip[65] = {0};
            strncpy(proof_tip, tip_ptr + 11, 64);
            char current_tip[JASH_BLAKE3_HEX_LEN];
            int tip_status = chain_tip_status(proof_tip, current_tip,
                                              proof_parse_chain_entries(body));
            if (tip_status == 0)
                jash_writes(w, ctx, "\033[1;32m✓ chain_tip matches live chain\033[0m\n");
            else if (tip_status == 3) {
                jash_writes(w, ctx,
                    "\033[1;33m✗ chain_tip in proof is obsolete\033[0m\n"
                    "  the chain grew past the proof; re-emit the proof to cover it\n");
                return 3;
            } else {
                jash_writes(w, ctx,
                    "\033[1;31m✗ chain_tip in proof differs from live chain\033[0m\n"
                    "  possible tampering or a different chain\n");
                return 2;
            }
        }
        return 0;
    } else {
        jash_writes(w, ctx, "\033[1;31m✗ proof signature INVALID\033[0m\n");
        return 1;
    }
}

static int cmd_proof_show(jmod_writer_t w, void* ctx, const char* inpath)
{
    if (!inpath || !*inpath) {
        inpath = sigil_identity_proof();
    }
    FILE* f = fopen(inpath, "r");
    if (!f) { jash_writef(w, ctx, "sigil: cannot open %s\n", inpath); return 1; }
    char line[256];
    while (fgets(line, sizeof(line), f)) jash_writes(w, ctx, line);
    fclose(f);
    return 0;
}

/* ── S3: log + blame ────────────────────────────────────────────────── */

static int cmd_log(jmod_writer_t w, void* ctx, const char* args)
{
    int tail = 20;
    if (args && strncmp(args, "--tail", 6) == 0) {
        const char* p = args + 6;
        while (*p == ' ') p++;
        if (*p) tail = atoi(p);
    }

    FILE* f = fopen(chain_path(), "r");
    if (!f) { jash_writes(w, ctx, "sigil: no chain (run `sigil log` to start)\n"); return 0; }

    char* lines = (char*)calloc(100, SIGIL_LINE_MAX);
    if (!lines) { fclose(f); return 1; }
    int  count = 0;
    char line[SIGIL_LINE_MAX];
    while (fgets(line, sizeof(line), f)) {
        const char* cp = strstr(line, " cmd=");
        if (cp && strncmp(cp + 5, "SESSION_END", 11) == 0) continue;
        char* slot = lines + (size_t)(count % 100) * SIGIL_LINE_MAX;
        snprintf(slot, SIGIL_LINE_MAX, "%s", line);
        count++;
    }
    fclose(f);

    int start = count > tail ? count - tail : 0;
    for (int i = start; i < count; i++) {
        char* l = lines + (size_t)(i % 100) * SIGIL_LINE_MAX;
        uint64_t seq = 0;
        char ts[32], hash[65];
        int rc = 0;
        if (sscanf(l, "%llu %31s %64s", (unsigned long long*)&seq, ts, hash) < 3) continue;
        const char* rcp = strstr(l, " rc=");
        if (rcp) sscanf(rcp, " rc=%d", &rc);
        const char* cmd_ptr = strstr(l, " cmd=");
        const char* cmd = cmd_ptr ? cmd_ptr + 5 : "?";
        char cmd_buf[256];
        strncpy(cmd_buf, cmd, 255); cmd_buf[255] = '\0';
        for (int j=0; cmd_buf[j]; j++) if(cmd_buf[j]=='\n') { cmd_buf[j]='\0'; break; }

        const char* rc_col = rc == 0 ? "\033[0m" : "\033[1;31m";
        uint64_t ts_sec = 0;
        sscanf(ts, "%llu", (unsigned long long*)&ts_sec);
        struct tm* tm = localtime((const time_t*)&ts_sec);
        char tstr[16] = "??:??:??";
        if (tm) snprintf(tstr, sizeof(tstr), "%02d:%02d:%02d",
                         tm->tm_hour, tm->tm_min, tm->tm_sec);

        jash_writef(w, ctx, "\033[90m#%llu %s %.8s\033[0m %src=%d\033[0m  %s\n",
                    (unsigned long long)seq, tstr, hash, rc_col, rc, cmd_buf);
    }
    free(lines);
    return 0;
}

static int cmd_blame(jmod_writer_t w, void* ctx, const char* pattern)
{
    if (!pattern || !*pattern) {
        jash_writes(w, ctx, "usage: sigil blame <pattern>\n");
        return 1;
    }

    FILE* f = fopen(chain_path(), "r");
    if (!f) { jash_writes(w, ctx, "sigil: no chain\n"); return 0; }

    char line[SIGIL_LINE_MAX];
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        const char* cmd_ptr = strstr(line, " cmd=");
        if (!cmd_ptr) continue;
        if (strstr(cmd_ptr + 5, pattern)) {
            uint64_t seq = 0;
            char ts[32], hash[65];
            int rc = 0;
            sscanf(line, "%llu %31s %64s", (unsigned long long*)&seq, ts, hash);
            const char* rcp = strstr(line, " rc=");
            if (rcp) sscanf(rcp, " rc=%d", &rc);
            const char* cmd = cmd_ptr + 5;
            char cmd_buf[256];
            strncpy(cmd_buf, cmd, 255); cmd_buf[255] = '\0';
            for (int j=0; cmd_buf[j]; j++) if(cmd_buf[j]=='\n'){cmd_buf[j]='\0';break;}

            uint64_t ts_sec = 0;
            sscanf(ts, "%llu", (unsigned long long*)&ts_sec);
            struct tm* tm = localtime((const time_t*)&ts_sec);
            char tstr[16] = "??:??:??";
            if (tm) snprintf(tstr, sizeof(tstr), "%02d:%02d:%02d",
                             tm->tm_hour, tm->tm_min, tm->tm_sec);

            const char* rc_col = rc==0 ? "\033[0m" : "\033[1;31m";
            jash_writef(w, ctx, "\033[90m#%llu %s\033[0m %src=%d\033[0m  %s\n",
                        (unsigned long long)seq, tstr, rc_col, rc, cmd_buf);
            found++;
        }
    }
    fclose(f);

    if (!found)
        jash_writef(w, ctx, "sigil blame: no commands matching '%s'\n", pattern);
    return 0;
}

/* ── S3: shared chain-line parser + diff/bisect ─────────────────────── */

typedef struct {
    uint64_t seq;
    uint64_t ts_sec;
    char     hash[65];
    int      rc;
    char     cmd[256];
    int      is_session_end;
} chain_rec_t;

static int parse_chain_line(const char* line, chain_rec_t* r)
{
    if (!line || !r) return 0;
    char ts[32] = {0};
    r->seq = 0; r->ts_sec = 0; r->rc = 0;
    r->hash[0] = '\0'; r->cmd[0] = '\0'; r->is_session_end = 0;
    if (sscanf(line, "%llu %31s %64s", (unsigned long long*)&r->seq, ts, r->hash) < 3)
        return 0;
    sscanf(ts, "%llu", (unsigned long long*)&r->ts_sec);
    const char* rcp = strstr(line, " rc=");
    if (rcp) sscanf(rcp, " rc=%d", &r->rc);
    const char* cp = strstr(line, " cmd=");
    if (cp) {
        strncpy(r->cmd, cp + 5, sizeof(r->cmd) - 1);
        r->cmd[sizeof(r->cmd) - 1] = '\0';
        for (int j = 0; r->cmd[j]; j++) if (r->cmd[j] == '\n') { r->cmd[j] = '\0'; break; }
        if (strncmp(cp + 5, "SESSION_END", 11) == 0) r->is_session_end = 1;
    }
    return 1;
}

static int find_chain_rec(uint64_t seq, chain_rec_t* out)
{
    FILE* f = fopen(chain_path(), "r");
    if (!f) return 0;
    char line[SIGIL_LINE_MAX];
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        chain_rec_t r;
        if (parse_chain_line(line, &r) && r.seq == seq) { *out = r; found = 1; break; }
    }
    fclose(f);
    return found;
}

static void fmt_hms(uint64_t ts_sec, char* out, size_t cap)
{
    struct tm* tm = localtime((const time_t*)&ts_sec);
    if (tm) snprintf(out, cap, "%02d:%02d:%02d", tm->tm_hour, tm->tm_min, tm->tm_sec);
    else    snprintf(out, cap, "??:??:??");
}

/* sigil diff <seqA> <seqB>  (or A..B) — honest diff of command text + rc.
 * The chain records the command + exit code, NOT filesystem state, so this is
 * a command/rc diff; cross-reference the undo timeline for file-level diffs. */
static int cmd_diff(jmod_writer_t w, void* ctx, const char* args)
{
    uint64_t a = 0, b = 0;
    if (!args ||
        (sscanf(args, "%llu %llu", (unsigned long long*)&a, (unsigned long long*)&b) < 2 &&
         sscanf(args, "%llu..%llu", (unsigned long long*)&a, (unsigned long long*)&b) < 2)) {
        jash_writes(w, ctx, "usage: sigil diff <seqA> <seqB>\n");
        return 1;
    }
    chain_rec_t ra, rb;
    if (!find_chain_rec(a, &ra)) { jash_writef(w, ctx, "sigil diff: seq #%llu not found\n", (unsigned long long)a); return 1; }
    if (!find_chain_rec(b, &rb)) { jash_writef(w, ctx, "sigil diff: seq #%llu not found\n", (unsigned long long)b); return 1; }
    char ta[16], tb[16];
    fmt_hms(ra.ts_sec, ta, sizeof ta);
    fmt_hms(rb.ts_sec, tb, sizeof tb);
    jash_writef(w, ctx, "\033[1m--- #%llu %s rc=%d\033[0m\n", (unsigned long long)a, ta, ra.rc);
    jash_writef(w, ctx, "\033[1m+++ #%llu %s rc=%d\033[0m\n", (unsigned long long)b, tb, rb.rc);
    jash_writef(w, ctx, "\033[31m- %s\033[0m\n", ra.cmd);
    jash_writef(w, ctx, "\033[32m+ %s\033[0m\n", rb.cmd);
    if (ra.rc != rb.rc) jash_writef(w, ctx, "rc: %d → %d\n", ra.rc, rb.rc);
    jash_writes(w, ctx, "\033[90m(chain records command + exit code; filesystem diff = undo timeline)\033[0m\n");
    return 0;
}

/* sigil bisect [good..bad | good bad] — read-only: report the FIRST recorded
 * failure (rc != 0) in the range (or whole chain). No re-execution (safe — no
 * side effects); predicate-replay bisect is a future libjef extension. */
static int cmd_bisect(jmod_writer_t w, void* ctx, const char* args)
{
    uint64_t lo = 0, hi = 0; int have_range = 0;
    if (args && *args) {
        if (sscanf(args, "%llu..%llu", (unsigned long long*)&lo, (unsigned long long*)&hi) == 2 ||
            sscanf(args, "%llu %llu",  (unsigned long long*)&lo, (unsigned long long*)&hi) == 2)
            have_range = 1;
    }
    FILE* f = fopen(chain_path(), "r");
    if (!f) { jash_writes(w, ctx, "sigil: no chain\n"); return 1; }
    char line[SIGIL_LINE_MAX];
    chain_rec_t ff; int found = 0; uint64_t scanned = 0;
    while (fgets(line, sizeof(line), f)) {
        chain_rec_t r;
        if (!parse_chain_line(line, &r) || r.is_session_end) continue;
        if (have_range && (r.seq < lo || r.seq > hi)) continue;
        scanned++;
        if (r.rc != 0) { ff = r; found = 1; break; }
    }
    fclose(f);
    if (!found) {
        jash_writef(w, ctx, "sigil bisect: no failing command in %s (%llu scanned) — all green\n",
                    have_range ? "range" : "chain", (unsigned long long)scanned);
        return 0;
    }
    char ts[16]; fmt_hms(ff.ts_sec, ts, sizeof ts);
    jash_writef(w, ctx, "\033[1;31mfirst failure:\033[0m #%llu %s rc=%d\n  %s\n",
                (unsigned long long)ff.seq, ts, ff.rc, ff.cmd);
    return 0;
}

/* ── S5: caps list ──────────────────────────────────────────────────── */

static const struct { uint32_t bit; const char* name; } CAP_NAMES[] = {
    { JMOD_CAP_FS_READ,       "fs_read"       },
    { JMOD_CAP_FS_WRITE,      "fs_write"      },
    { JMOD_CAP_NET,           "net"           },
    { JMOD_CAP_EXEC,          "exec"          },
    { JMOD_CAP_STATE_READ,    "state_read"    },
    { JMOD_CAP_STATE_WRITE,   "state_write"   },
    { JMOD_CAP_TTY,           "tty"           },
    { JMOD_CAP_CRYPTO,        "crypto"        },
    { JMOD_CAP_MODULE_LOAD,   "module_load"   },
    { JMOD_CAP_HOOK_PRE_CMD,  "hook_pre_cmd"  },
    { JMOD_CAP_HOOK_POST_CMD, "hook_post_cmd" },
    { JMOD_CAP_HOOK_EXIT,     "hook_exit"     },
    { JMOD_CAP_HOOK_MUTATION, "hook_mutation" },
    { 0, NULL }
};

static void caps_str(uint32_t caps, char* buf, size_t cap)
{
    buf[0] = '\0';
    size_t pos = 0;
    for (int i = 0; CAP_NAMES[i].name; i++) {
        if (caps & CAP_NAMES[i].bit) {
            size_t nl = strlen(CAP_NAMES[i].name);
            if (pos + nl + 2 < cap) {
                if (pos) { buf[pos++] = ','; buf[pos++] = ' '; }
                memcpy(buf + pos, CAP_NAMES[i].name, nl); pos += nl;
                buf[pos] = '\0';
            }
        }
    }
}

typedef struct { jmod_writer_t w; void* ctx; } caps_cb_state_t;

static void caps_cb(const jmod_entry_t* e, void* udata)
{
    caps_cb_state_t* s = (caps_cb_state_t*)udata;
    char decl_str[256], used_str[256];
    caps_str(e->caps_declared, decl_str, sizeof(decl_str));
    caps_str(e->caps_used, used_str, sizeof(used_str));
    int mismatch = (e->caps_used & ~e->caps_declared) != 0;
    const char* status = mismatch ? "\033[1;31m[MISMATCH]\033[0m" : "\033[1;32m[OK]\033[0m";
    jash_writef(s->w, s->ctx, "%-16s v%-10s %s\n"
                "  declared: 0x%04x  [%s]\n"
                "  used:     0x%04x  [%s]\n",
                e->name, e->version, status,
                e->caps_declared, decl_str[0] ? decl_str : "none",
                e->caps_used,     used_str[0]  ? used_str  : "none");
}

static int cmd_caps(jmod_writer_t w, void* ctx)
{
    jash_writes(w, ctx, "\033[1mLoaded module capabilities:\033[0m\n"
                        "────────────────────────────────────────────────\n");
    caps_cb_state_t st = { w, ctx };
    jmod_each(caps_cb, &st);
    return 0;
}

/* ── S6: OCI artifact export + push ────────────────────────────────── */

static long read_file_alloc(const char* path, uint8_t** out)
{
    *out = NULL;
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return -1;
    struct stat before, after;
    int error = 0;
    if (fstat(fd, &before)) error = errno;
    else if (!S_ISREG(before.st_mode) || before.st_size <= 0) error = EINVAL;
    else if ((uint64_t)before.st_size > 32u * 1024u * 1024u) error = EFBIG;
    size_t size = error ? 0 : (size_t)before.st_size;
    uint8_t* data = error ? NULL : malloc(size + 1);
    if (!error && !data) error = ENOMEM;
    size_t used = 0;
    while (!error && used <= size) {
        ssize_t count = read(fd, data + used, size + 1 - used);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) { error = errno; break; }
        if (!count) break;
        used += (size_t)count;
        if (used > size) { error = ESTALE; break; }
    }
    if (!error && used != size) error = ESTALE;
    if (!error && fstat(fd, &after)) error = errno;
    if (!error && (before.st_dev != after.st_dev || before.st_ino != after.st_ino ||
        before.st_size != after.st_size || before.st_mtime != after.st_mtime || before.st_ctime != after.st_ctime
#if defined(__linux__)
        || before.st_mtim.tv_nsec != after.st_mtim.tv_nsec || before.st_ctim.tv_nsec != after.st_ctim.tv_nsec
#endif
        )) error = ESTALE;
    if (close(fd) && !error) error = errno;
    if (error) { free(data); errno = error; return -1; }
    *out = data; return (long)size;
}

static int write_blob(const char* blobs_dir, const uint8_t* data, size_t len,
                       char hex_out[JASH_SHA256_HEX_LEN])
{
    /* OCI blobs use SHA-256 per the OCI image spec */
    uint8_t d[JASH_SHA256_DIGEST_LEN];
    jash_sha256(data, len, d);
    jash_sha256_hex(d, hex_out);
    char path[PATH_MAX];
    if (snprintf(path, sizeof path, "%s/%s", blobs_dir, hex_out) >=
        (int)sizeof path)
        return -1;
    FILE* f = fopen(path, "wb");
    if (!f) return -1;
    int ok = fwrite(data, 1, len, f) == len;
    if (fclose(f) != 0) ok = 0;
    return ok ? 0 : -1;
}

static int mkdir_p(const char* dir)
{
    char tmp[PATH_MAX];
    if (snprintf(tmp, sizeof tmp, "%s", dir) >= (int)sizeof tmp) return -1;
    for (char* p = tmp+1; *p; p++) {
        if (*p=='/') {
            *p='\0';
            if (mkdir(tmp,0700) != 0 && errno != EEXIST) return -1;
            *p='/';
        }
    }
    return mkdir(tmp, 0700) == 0 || errno == EEXIST ? 0 : -1;
}

typedef struct {
    char directory[PATH_MAX], chain_sha256[65], manifest_sha256[65];
    uint64_t chain_entries, chain_bytes;
    int effects_started, layout_written;
    const char* error;
} sigil_export_observation_t;

static int cmd_export_observed(jmod_writer_t w, void* ctx, const char* args,
                               sigil_export_observation_t* observation)
{
    if (observation) { memset(observation, 0, sizeof *observation); observation->error = "export_preparation_failed"; }
    sigil_export_args_t av = {0};
    if (sigil_export_args_parse(args, &av) < 0) {
        jash_writes(w, ctx, "sigil export: invalid quoting\n");
        return 2;
    }
    const char* out_arg = NULL;
    const char* manifest_path = NULL;
    for (int i = 0; i < av.argc; i++) {
        const char* arg = av.argv[i];
        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            jash_writes(w, ctx,
                "usage: sigil export [directory] [--manifest path]\n"
                "  --manifest signs the exact caller-manifest bytes in DSSE.\n");
            sigil_export_args_free(&av);
            return 0;
        }
        if (strcmp(arg, "--manifest") == 0) {
            if (manifest_path || i + 1 >= av.argc) {
                jash_writes(w, ctx, "sigil export: --manifest requires one path\n");
                sigil_export_args_free(&av);
                return 2;
            }
            manifest_path = av.argv[++i];
            continue;
        }
        if (arg[0] == '-') {
            jash_writef(w, ctx, "sigil export: unknown option: %s\n", arg);
            sigil_export_args_free(&av);
            return 2;
        }
        if (out_arg) {
            jash_writes(w, ctx, "sigil export: only one output directory is allowed\n");
            sigil_export_args_free(&av);
            return 2;
        }
        out_arg = arg;
    }

    (void)sigil_identity_home();
    if (!g_ledger.has_home) {
        jash_writes(w, ctx, "sigil export: HOME not set\n");
        sigil_export_args_free(&av);
        return 1;
    }

    char out_dir[PATH_MAX];
    int out_len = out_arg
        ? snprintf(out_dir, sizeof out_dir, "%s", out_arg)
        : snprintf(out_dir, sizeof out_dir, "%s/oci-export", g_ledger.jash_dir);
    if (out_len < 0 || out_len >= (int)sizeof out_dir) {
        jash_writes(w, ctx, "sigil export: output path exceeds PATH_MAX\n");
        sigil_export_args_free(&av);
        return 1;
    }
    if (observation) memcpy(observation->directory, out_dir, (size_t)out_len + 1);

    const char* chain_src = chain_path();
    const char* proof_src = sigil_identity_proof();

    uint8_t* proof_data = NULL; long proof_sz  = read_file_alloc(proof_src, &proof_data);
    if (proof_sz < 0 && errno != ENOENT) {
        if (observation) observation->error = "proof_read_failed";
        jash_writes(w, ctx, "sigil export: existing proof is unreadable or invalid\n");
        sigil_export_args_free(&av); return 1;
    }
    uint8_t* caller_manifest = NULL;
    long caller_manifest_sz = manifest_path
        ? read_file_alloc(manifest_path, &caller_manifest) : -1;

    if (manifest_path && caller_manifest_sz < 0) {
        free(proof_data);
        jash_writef(w, ctx, "sigil export: cannot read manifest: %s\n", manifest_path);
        sigil_export_args_free(&av);
        return 1;
    }

    /* One immutable snapshot supplies health, bytes, count, and tip. Without
     * the shared lock, a concurrent append could make those fields describe
     * different moments while each still looked individually plausible. */
    if (cmd_chain_verify_range(sigil_discard_writer, NULL, 0, 0, 0) != 0) {
        free(proof_data); free(caller_manifest);
        jash_writes(w, ctx, "sigil export: active chain is not healthy\n");
        sigil_export_args_free(&av);
        return 1;
    }
    int export_transaction_fd = -1;
    FILE* chain_guard = sigil_verify_snapshot_open(&export_transaction_fd);
    if (!chain_guard) {
        free(proof_data); free(caller_manifest);
        jash_writes(w, ctx, "sigil export: cannot lock active chain\n");
        sigil_export_args_free(&av);
        return 1;
    }

    uint8_t* chain_data = NULL;
    long chain_sz = read_file_alloc(chain_src, &chain_data);
    if (chain_sz < 0) {
        sigil_verify_snapshot_close(chain_guard, export_transaction_fd);
        free(proof_data); free(caller_manifest);
        jash_writes(w, ctx, "sigil export: no healthy chain to export\n");
        sigil_export_args_free(&av);
        return 1;
    }

    char chain_hex[JASH_SHA256_HEX_LEN];
    uint8_t chain_digest[JASH_SHA256_DIGEST_LEN];
    jash_sha256(chain_data, (size_t)chain_sz, chain_digest);
    jash_sha256_hex(chain_digest, chain_hex);
    uint64_t n_entries = chain_count_entries();
    if (observation) {
        observation->chain_entries = n_entries;
        observation->chain_bytes = (uint64_t)chain_sz;
        memcpy(observation->chain_sha256, chain_hex, sizeof observation->chain_sha256);
    }
    sigil_chain_scan_t chain_scan;
    if (sigil_scan_chain(chain_src, &chain_scan) != 0 || !chain_scan.last_s7[0]) {
        sigil_verify_snapshot_close(chain_guard, export_transaction_fd);
        free(chain_data); free(proof_data); free(caller_manifest);
        jash_writes(w, ctx, "sigil export: cannot identify chain tip\n");
        sigil_export_args_free(&av);
        return 1;
    }
    unsigned long long tip_seq = 0;
    char tip_ts[32], chain_tip[65];
    if (sscanf(chain_scan.last_s7, "%llu %31s %64s", &tip_seq, tip_ts,
               chain_tip) != 3 || !sigil_hex64(chain_tip)) {
        sigil_verify_snapshot_close(chain_guard, export_transaction_fd);
        free(chain_data); free(proof_data); free(caller_manifest);
        jash_writes(w, ctx, "sigil export: malformed chain tip\n");
        sigil_export_args_free(&av);
        return 1;
    }
    sigil_verify_snapshot_close(chain_guard, export_transaction_fd);

    char caller_manifest_hex[JASH_SHA256_HEX_LEN] = {0};
    uint8_t* dsse_data = NULL;
    size_t dsse_sz = 0;
    if (caller_manifest) {
        uint8_t digest[JASH_SHA256_DIGEST_LEN];
        jash_sha256(caller_manifest, (size_t)caller_manifest_sz, digest);
        jash_sha256_hex(digest, caller_manifest_hex);
        char payload[1024];
        int payload_len = snprintf(payload, sizeof payload,
            "{\"schema_version\":1,"
            "\"bearcode_manifest_sha256\":\"%s\","
            "\"bearcode_manifest_media_type\":\"%s\","
            "\"manifest_size\":%ld,\"chain_sha256\":\"%s\","
            "\"chain_tip\":\"%s\",\"chain_entries\":%llu}\n",
            caller_manifest_hex, BEARCODE_MANIFEST_MEDIA_TYPE,
            caller_manifest_sz, chain_hex, chain_tip,
            (unsigned long long)n_entries);
        if (payload_len < 0 || (size_t)payload_len >= sizeof payload ||
            dsse_envelope_alloc(SIGIL_EXPORT_PAYLOAD_TYPE,
                                (const uint8_t*)payload, (size_t)payload_len,
                                &dsse_data, &dsse_sz) != 0) {
            free(chain_data); free(proof_data); free(caller_manifest);
            jash_writes(w, ctx, "sigil export: could not sign manifest binding\n");
            sigil_export_args_free(&av);
            return 1;
        }
    }

    char blobs_dir[PATH_MAX];
    if (observation) { observation->effects_started = 1; observation->error = "export_write_failed"; }
    if (snprintf(blobs_dir, sizeof blobs_dir, "%s/blobs/sha256", out_dir) >=
            (int)sizeof blobs_dir || mkdir_p(blobs_dir) != 0)
        goto export_write_fail;

    char tmp[PATH_MAX];
    if (snprintf(tmp, sizeof tmp, "%s/oci-layout", out_dir) >= (int)sizeof tmp)
        goto export_write_fail;
    {
        const char* cl = "{\"imageLayoutVersion\":\"1.0.0\"}\n";
        FILE* f = fopen(tmp, "w");
        int ok = f && fputs(cl, f) != EOF;
        if (f && fclose(f) != 0) ok = 0;
        if (!ok) goto export_write_fail;
    }

    const uint8_t cfg[] = "{}"; size_t cfg_sz = 2;
    char cfg_hex[JASH_SHA256_HEX_LEN];
    if (write_blob(blobs_dir, cfg, cfg_sz, cfg_hex) != 0)
        goto export_write_fail;

    if (write_blob(blobs_dir, chain_data, (size_t)chain_sz, chain_hex) != 0)
        goto export_write_fail;
    free(chain_data); chain_data = NULL;

    char proof_hex[JASH_SHA256_HEX_LEN] = {0};
    if (proof_data && proof_sz > 0) {
        if (write_blob(blobs_dir, proof_data, (size_t)proof_sz, proof_hex) != 0)
            goto export_write_fail;
        free(proof_data); proof_data = NULL;
    }

    char caller_blob_hex[JASH_SHA256_HEX_LEN] = {0};
    if (caller_manifest &&
        write_blob(blobs_dir, caller_manifest, (size_t)caller_manifest_sz,
                   caller_blob_hex) != 0)
        goto export_write_fail;
    free(caller_manifest); caller_manifest = NULL;

    char dsse_hex[JASH_SHA256_HEX_LEN] = {0};
    if (dsse_data && write_blob(blobs_dir, dsse_data, dsse_sz, dsse_hex) != 0)
        goto export_write_fail;
    free(dsse_data); dsse_data = NULL;

    char mj[8192]; int mlen = 0;
    mlen += snprintf(mj+mlen, sizeof(mj)-(size_t)mlen,
        "{\"schemaVersion\":2,"
        "\"mediaType\":\"application/vnd.oci.image.manifest.v1+json\","
        "\"artifactType\":\"application/vnd.jash.sigil.export.v1\",");
    if (caller_blob_hex[0]) {
        mlen += snprintf(mj+mlen, sizeof(mj)-(size_t)mlen,
            "\"subject\":{\"mediaType\":\"%s\",\"size\":%ld,"
            "\"digest\":\"sha256:%s\"},",
            BEARCODE_MANIFEST_MEDIA_TYPE, caller_manifest_sz, caller_blob_hex);
    }
    mlen += snprintf(mj+mlen, sizeof(mj)-(size_t)mlen,
        "\"config\":{\"mediaType\":\"application/vnd.oci.empty.v1+json\","
        "\"size\":%zu,\"digest\":\"sha256:%s\"},\"layers\":[",
        cfg_sz, cfg_hex);
    int first = 1;
    if (chain_hex[0]) {
        mlen += snprintf(mj+mlen, sizeof(mj)-(size_t)mlen,
            "{\"mediaType\":\"application/vnd.jash.chain.v1\","
            "\"size\":%ld,\"digest\":\"sha256:%s\"}", chain_sz, chain_hex);
        first = 0;
    }
    if (proof_hex[0]) {
        if (!first) mj[mlen++] = ',';
        mlen += snprintf(mj+mlen, sizeof(mj)-(size_t)mlen,
            "{\"mediaType\":\"application/vnd.jash.proof.v1\","
            "\"size\":%ld,\"digest\":\"sha256:%s\"}", proof_sz, proof_hex);
        first = 0;
    }
    if (caller_blob_hex[0]) {
        if (!first) mj[mlen++] = ',';
        mlen += snprintf(mj+mlen, sizeof(mj)-(size_t)mlen,
            "{\"mediaType\":\"%s\",\"size\":%ld,"
            "\"digest\":\"sha256:%s\"}",
            BEARCODE_MANIFEST_MEDIA_TYPE, caller_manifest_sz, caller_blob_hex);
        first = 0;
    }
    if (dsse_hex[0]) {
        if (!first) mj[mlen++] = ',';
        mlen += snprintf(mj+mlen, sizeof(mj)-(size_t)mlen,
            "{\"mediaType\":\"application/vnd.dsse.envelope.v1+json\","
            "\"size\":%zu,\"digest\":\"sha256:%s\"}", dsse_sz, dsse_hex);
    }
    mlen += snprintf(mj+mlen, sizeof(mj)-(size_t)mlen,
        "],\"annotations\":{"
        "\"org.jash.chain.entries\":\"%llu\","
        "\"org.jash.sigil.level\":\"%d\","
        "\"org.jash.proof.status\":\"%s\"}}\n",
        (unsigned long long)n_entries, jash_sigil_get(),
        dsse_hex[0] || proof_hex[0] ? "signed" : "unsigned");

    if (mlen < 0 || (size_t)mlen >= sizeof mj) goto export_write_fail;

    char manifest_hex[JASH_SHA256_HEX_LEN];
    if (write_blob(blobs_dir, (const uint8_t*)mj, (size_t)mlen,
                   manifest_hex) != 0) goto export_write_fail;

    char ij[1024]; int ilen;
    ilen = snprintf(ij, sizeof(ij),
        "{\"schemaVersion\":2,"
        "\"mediaType\":\"application/vnd.oci.image.index.v1+json\","
        "\"manifests\":[{"
        "\"mediaType\":\"application/vnd.oci.image.manifest.v1+json\","
        "\"size\":%d,\"digest\":\"sha256:%s\","
        "\"annotations\":{\"org.opencontainers.image.ref.name\":\"latest\"}"
        "}]}\n", mlen, manifest_hex);
    if (snprintf(tmp, sizeof tmp, "%s/index.json", out_dir) >= (int)sizeof tmp)
        goto export_write_fail;
    {
        FILE* f = fopen(tmp, "w");
        int ok = f && fwrite(ij, 1, (size_t)ilen, f) == (size_t)ilen;
        if (f && fclose(f) != 0) ok = 0;
        if (!ok) goto export_write_fail;
    }

    char cs[17]; snprintf(cs, sizeof(cs), "%.16s", chain_hex[0]?chain_hex:"(no chain)");
    if (observation) {
        memcpy(observation->manifest_sha256, manifest_hex, sizeof observation->manifest_sha256);
        observation->layout_written = 1;
        observation->error = NULL;
    }
    char ms[17]={0}; memcpy(ms, manifest_hex, 16);
    jash_writef(w, ctx,
        "OCI export → %s\n"
        "  entries: %llu   chain: sha256:%s...\n"
        "  manifest: sha256:%s...\n"
        "Push: oras push <registry>/<repo>:latest --layout %s\n",
        out_dir, (unsigned long long)n_entries, cs, ms, out_dir);
    sigil_export_args_free(&av);
    return 0;

export_write_fail:
    free(chain_data); free(proof_data); free(caller_manifest); free(dsse_data);
    jash_writes(w, ctx, "sigil export: failed to write OCI layout\n");
    sigil_export_args_free(&av);
    return 1;
}

static int cmd_export(jmod_writer_t w, void* ctx, const char* args)
{
    return cmd_export_observed(w, ctx, args, NULL);
}

static int sigil_registry_ref_valid(const char* ref)
{
    if (!ref || !*ref || strlen(ref) > 255 || !strchr(ref, '/')) return 0;
    if (ref[0] == '/' || ref[strlen(ref) - 1] == '/' || strstr(ref, "//") ||
        strstr(ref, "..") || strchr(ref, ':'))
        return 0;
    for (const unsigned char* p = (const unsigned char*)ref; *p; p++) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
            (*p >= '0' && *p <= '9') || *p == '.' || *p == '_' ||
            *p == '-' || *p == '/')
            continue;
        return 0;
    }
    return 1;
}

/* Execute oras without a command interpreter. argv carries the registry
 * reference and layout path as data, so shell metacharacters cannot execute. */
static int sigil_run_oras_push(jmod_writer_t w, void* ctx,
                               const char* ref, const char* layout)
{
    int pipefd[2];
    if (pipe(pipefd) != 0) return -1;
    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]); close(pipefd[1]);
        return -1;
    }
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        execlp("oras", "oras", "push", ref, "--layout", layout, (char*)NULL);
        _exit(127);
    }
    close(pipefd[1]);
    char line[512];
    FILE* out = fdopen(pipefd[0], "r");
    if (out) {
        while (fgets(line, sizeof(line), out)) jash_writes(w, ctx, line);
        fclose(out);
    } else {
        close(pipefd[0]);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
}

static int cmd_push(jmod_writer_t w, void* ctx, const char* args)
{
    if (!args || !*args) {
        jash_writes(w, ctx,
            "usage: sigil push <registry>/<repo>[:tag]\n"
            "  Exports OCI layout then calls `oras push`.\n"
            "  Install oras: https://oras.land\n");
        return 1;
    }

    if (!sigil_registry_ref_valid(args)) {
        jash_writef(w, ctx,
            "sigil push: invalid registry reference '%s'\n"
            "  expected: <registry>/<repository> using [A-Za-z0-9._/-]\n",
            args);
        return 1;
    }

    int rc = cmd_export(w, ctx, "");
    if (rc != 0) return rc;

    (void)sigil_identity_home();
    if (!g_ledger.has_home) return 1;
    char export_dir[PATH_MAX];
    if (snprintf(export_dir, sizeof export_dir, "%s/oci-export",
                 g_ledger.jash_dir) >= (int)sizeof export_dir) {
        jash_writes(w, ctx, "sigil push: export path exceeds PATH_MAX\n");
        return 1;
    }

    char ref[320];
    snprintf(ref, sizeof(ref), "%s:latest", args);
    jash_writef(w, ctx, "→ oras push %s --layout %s\n", ref, export_dir);
    int ec = sigil_run_oras_push(w, ctx, ref, export_dir);
    if (ec == 127) {
        jash_writes(w, ctx, "sigil push: oras not found — install from https://oras.land\n");
        return 1;
    }
    if (ec == 0)
        jash_writef(w, ctx, "\033[1;32m✓ pushed %s\033[0m\n", ref);
    return ec == 0 ? 0 : 1;
}

/* ── S4: sealed restore + adversarial battery ───────────────────────── */

typedef struct {
    uint64_t seq;
    uint64_t ts_ns;
    int      rc;
    char     hash[65];
    char     cmd[512];
} chain_entry_t;

static int chain_read_entry(uint64_t target_seq, chain_entry_t* out)
{
    FILE* f = fopen(chain_path(), "r");
    if (!f) return -1;
    char line[SIGIL_LINE_MAX];
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        uint64_t seq = 0; char ts[32], hash[65]; int rc = 0;
        if (sscanf(line, "%llu %31s %64s",
                   (unsigned long long*)&seq, ts, hash) < 3) continue;
        if (seq != target_seq) continue;
        const char* rcp = strstr(line, " rc=");
        if (!rcp || sscanf(rcp, " rc=%d", &rc) < 1) continue;
        out->seq = seq;
        uint64_t ts_sec = 0, ts_nsec = 0;
        sscanf(ts, "%llu.%llu", (unsigned long long*)&ts_sec, (unsigned long long*)&ts_nsec);
        out->ts_ns = ts_sec * 1000000000ULL + ts_nsec;
        out->rc = rc;
        strncpy(out->hash, hash, 64); out->hash[64] = '\0';
        const char* cp = strstr(line, " cmd=");
        if (cp) {
            strncpy(out->cmd, cp + 5, sizeof(out->cmd)-1); out->cmd[sizeof(out->cmd)-1] = '\0';
            for (int j=0; out->cmd[j]; j++) if(out->cmd[j]=='\n'){out->cmd[j]='\0';break;}
        }
        found = 1; break;
    }
    fclose(f);
    return found ? 0 : -1;
}

/* Attack 1: Hash Forgery — recompute BLAKE3 chain to target_seq and compare */
static int attack_hash_forgery(uint64_t target_seq, const char* expected_hash)
{
    FILE* f = fopen(chain_path(), "r");
    if (!f) return -1;
    char line[SIGIL_LINE_MAX];
    uint8_t running_hash[32];
    memset(running_hash, 0, sizeof(running_hash));
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        uint64_t seq = 0; char ts[32], hash[65]; int rc = 0;
        if (sscanf(line, "%llu %31s %64s",
                   (unsigned long long*)&seq, ts, hash) < 3) continue;
        const char* rcp = strstr(line, " rc=");
        if (!rcp || sscanf(rcp, " rc=%d", &rc) < 1) continue;
        uint64_t ts_sec = 0, ts_nsec = 0;
        sscanf(ts, "%llu.%llu", (unsigned long long*)&ts_sec, (unsigned long long*)&ts_nsec);
        uint64_t ts_ns = ts_sec * 1000000000ULL + ts_nsec;
        const char* cp = strstr(line, " cmd="); const char* cmd = cp ? cp + 5 : "";
        size_t clen = strlen(cmd); if (clen && cmd[clen-1]=='\n') clen--;
        uint8_t payload[4096]; size_t plen = 0;
        for (int i=7;i>=0;i--) { payload[plen++]=(uint8_t)(ts_ns>>(i*8)); }
        payload[plen++]=(uint8_t)(rc&0xFF); payload[plen++]=(uint8_t)((rc>>8)&0xFF);
        payload[plen++]=(uint8_t)((rc>>16)&0xFF); payload[plen++]=(uint8_t)((rc>>24)&0xFF);
        if (clen>SIGIL_PAYLOAD_CMD_MAX(plen)) clen=SIGIL_PAYLOAD_CMD_MAX(plen);
        if (clen) { memcpy(payload+plen,cmd,clen); plen+=clen; }
        uint8_t new_hash[32];
        jash_blake3_chain(running_hash, payload, plen, new_hash);
        memcpy(running_hash, new_hash, 32);
        if (seq == target_seq) {
            char computed[JASH_BLAKE3_HEX_LEN]; jash_blake3_hex(new_hash, computed);
            found = (strcmp(computed, expected_hash) == 0); break;
        }
    }
    fclose(f);
    return found ? 0 : -1;
}

/* Attack 2: Timestamp Sanity */
static int attack_timestamp(uint64_t ts_ns)
{
    uint64_t n = now_ns();
    if (ts_ns > n + 2000000000ULL) return -1;
    return 0;
}

/* Attack 3: Sequence Continuity */
static int attack_sequence_gap(uint64_t target_seq)
{
    FILE* f = fopen(chain_path(), "r");
    if (!f) return -1;
    char line[SIGIL_LINE_MAX]; uint64_t expected = 0; int ok = 1;
    while (fgets(line, sizeof(line), f)) {
        uint64_t seq = 0; char ts[32], hash[65]; int rc = 0;
        if (sscanf(line, "%llu %31s %64s",
                   (unsigned long long*)&seq, ts, hash) < 3) continue;
        const char* rcp = strstr(line, " rc=");
        if (!rcp || sscanf(rcp, " rc=%d", &rc) < 1) continue;
        if (seq != expected) { ok = 0; break; }
        expected++;
        if (seq == target_seq) break;
    }
    fclose(f);
    return ok ? 0 : -1;
}

/* Attack 4: Cross-Session Tamper */
static int attack_proof_signature(void)
{
    FILE* f = fopen(sigil_identity_proof(), "r");
    if (!f) return 0;
    char body[2048] = {0}; char stored_sig[65] = {0};
    char line[256]; size_t body_len = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "sig: ", 5) == 0) {
            size_t siglen = strcspn(line + 5, "\r\n");
            if (siglen != 64) { fclose(f); return -1; }
            memcpy(stored_sig, line + 5, siglen);
            stored_sig[siglen] = '\0';
            break;
        }
        size_t ll = strlen(line);
        if (body_len + ll < sizeof(body)) { memcpy(body+body_len, line, ll); body_len += ll; }
    }
    fclose(f);
    if (!stored_sig[0]) return 0;
    uint8_t key[32];
    if (jash_session_key_load(key) != 0) return 0;
    uint8_t mac[32]; char mac_hex[JASH_BLAKE3_HEX_LEN];
    int is_blake3 = (strstr(body, "sig_algo: blake3-keyed") != NULL);
    if (is_blake3) {
        jash_blake3_keyed(key, (const uint8_t*)body, body_len, mac);
        jash_blake3_hex(mac, mac_hex);
    } else {
        jash_hmac_sha256(key, 32, (const uint8_t*)body, body_len, mac);
        jash_sha256_hex(mac, mac_hex);
    }
    return (strcmp(mac_hex, stored_sig) == 0) ? 0 : -1;
}

static int cmd_restore(jmod_writer_t w, void* ctx, const char* args)
{
    if (!args || !*args) {
        jash_writes(w, ctx, "usage: sigil restore <seq>\n"); return 1;
    }
    uint64_t target_seq = (uint64_t)strtoull(args, NULL, 10);
    chain_entry_t entry; memset(&entry, 0, sizeof(entry));
    if (chain_read_entry(target_seq, &entry) != 0) {
        jash_writef(w, ctx, "sigil restore: seq %llu not found\n",
                    (unsigned long long)target_seq); return 1;
    }
    uint64_t ts_sec = entry.ts_ns / 1000000000ULL;
    struct tm* tm = localtime((const time_t*)&ts_sec);
    char tstr[32] = "??:??:??";
    if (tm) snprintf(tstr,sizeof(tstr),"%02d:%02d:%02d",tm->tm_hour,tm->tm_min,tm->tm_sec);

    jash_writef(w, ctx, "\033[1mrestore #%llu\033[0m  %s  rc=%d\n  cmd: %s\n\n",
                (unsigned long long)target_seq, tstr, entry.rc, entry.cmd);

    if (jash_sigil_get() < SIGIL_SEAL) {
        jash_writes(w, ctx, "\033[90m(seal mode off — battery skipped; use `sigil seal` to enable)\033[0m\n");
        return 0;
    }

    jash_writes(w, ctx, "\033[1mAdversarial Battery — 4 attacks\033[0m\n");
    jash_writes(w, ctx, "──────────────────────────────────────────────\n");
    int all_pass = 1;

    int a1 = attack_hash_forgery(target_seq, entry.hash);
    if (a1 == 0)
        jash_writef(w, ctx, "\033[1;32m[REFUTED]\033[0m  1. Hash Forgery\n"
                    "           H(%llu) recomputed → matches chain\n",
                    (unsigned long long)target_seq);
    else {
        jash_writef(w, ctx, "\033[1;31m[CONFIRMED]\033[0m 1. Hash Forgery\n"
                    "           hash mismatch at seq %llu\n",
                    (unsigned long long)target_seq);
        all_pass = 0;
    }

    int a2 = attack_timestamp(entry.ts_ns);
    if (a2 == 0)
        jash_writef(w, ctx, "\033[1;32m[REFUTED]\033[0m  2. Timestamp Replay\n"
                    "           ts=%s is within session window\n", tstr);
    else {
        jash_writef(w, ctx, "\033[1;31m[CONFIRMED]\033[0m 2. Timestamp Replay\n"
                    "           ts=%s anomalous (future-dated)\n", tstr);
        all_pass = 0;
    }

    int a3 = attack_sequence_gap(target_seq);
    if (a3 == 0)
        jash_writef(w, ctx, "\033[1;32m[REFUTED]\033[0m  3. Sequence Gap\n"
                    "           seq 0..%llu contiguous\n",
                    (unsigned long long)target_seq);
    else {
        jash_writef(w, ctx, "\033[1;31m[CONFIRMED]\033[0m 3. Sequence Gap\n"
                    "           discontinuity before seq %llu\n",
                    (unsigned long long)target_seq);
        all_pass = 0;
    }

    int a4 = attack_proof_signature();
    if (a4 == 0)
        jash_writes(w, ctx, "\033[1;32m[REFUTED]\033[0m  4. Cross-Session Tamper\n"
                    "           proof MAC valid (or absent — clean session)\n");
    else {
        jash_writes(w, ctx, "\033[1;31m[CONFIRMED]\033[0m 4. Cross-Session Tamper\n"
                    "           proof MAC mismatch — chain from different session?\n");
        all_pass = 0;
    }

    jash_writes(w, ctx, "──────────────────────────────────────────────\n");
    if (all_pass) {
        jash_writes(w, ctx, "\033[1;32m✓ BATTERY PASSED — restore authorized\033[0m\n");
        jash_writef(w, ctx, "  $ %s\n", entry.cmd);
    } else {
        jash_writes(w, ctx, "\033[1;31m✗ BATTERY FAILED — restore blocked\033[0m\n"
                    "  Chain integrity not guaranteed; investigate before executing.\n");
    }
    return all_pass ? 0 : 1;
}

/* ── S9: Determinism replay ─────────────────────────────────────────── */

static int cmd_replay(jmod_writer_t w, void* ctx, const char* args)
{
    if (!args || !*args) {
        jash_writes(w, ctx,
            "usage: sigil replay [--exec] <seq>\n"
            "       sigil replay [--exec] <a>..<b>\n"
            "  Without --exec: show commands from chain (no execution).\n"
            "  With    --exec: re-execute each command and compare exit code.\n");
        return 1;
    }

    int do_exec = 0;
    const char* p = args;
    if (strncmp(p, "--exec", 6) == 0 && (p[6]==' '||p[6]=='\0')) {
        do_exec = 1;
        p += 6; while (*p == ' ') p++;
    }

    uint64_t seq_from = 0, seq_to = 0;
    int is_range = 0;
    if (sscanf(p, "%llu..%llu", (unsigned long long*)&seq_from,
                                 (unsigned long long*)&seq_to) == 2)
        is_range = 1;
    else
        seq_from = seq_to = (uint64_t)strtoull(p, NULL, 10);

    int any_found = 0, non_det = 0;
    for (uint64_t s = seq_from; s <= seq_to; s++) {
        chain_entry_t entry; memset(&entry, 0, sizeof(entry));
        if (chain_read_entry(s, &entry) != 0) {
            jash_writef(w, ctx, "sigil replay: seq %llu not found\n",
                        (unsigned long long)s);
            continue;
        }
        any_found = 1;

        uint64_t ts_sec = entry.ts_ns / 1000000000ULL;
        struct tm* tm = localtime((const time_t*)&ts_sec);
        char tstr[32] = "??:??:??";
        if (tm) snprintf(tstr,sizeof(tstr),"%02d:%02d:%02d",
                         tm->tm_hour,tm->tm_min,tm->tm_sec);

        if (!do_exec) {
            jash_writef(w, ctx, "\033[90m#%llu %s rc=%d\033[0m  %s\n",
                        (unsigned long long)s, tstr, entry.rc, entry.cmd);
            continue;
        }

        jash_writef(w, ctx, "\033[90m#%llu\033[0m $ %s\n",
                    (unsigned long long)s, entry.cmd);
        int wstatus = system(entry.cmd);
        int actual_rc = (wstatus != -1 && WIFEXITED(wstatus))
                      ? WEXITSTATUS(wstatus) : -1;

        if (actual_rc == entry.rc)
            jash_writef(w, ctx,
                "\033[1;32m✓ DETERMINISTIC\033[0m  rc=%d matches stored\n",
                actual_rc);
        else {
            jash_writef(w, ctx,
                "\033[1;31m✗ NON-DETERMINISTIC\033[0m  rc=%d ≠ stored rc=%d\n",
                actual_rc, entry.rc);
            non_det = 1;
        }
    }

    if (!any_found) return 1;
    if (is_range && do_exec) {
        if (non_det)
            jash_writef(w, ctx,
                "\033[1;31m⚠ replay %llu..%llu: non-deterministic entries\033[0m\n",
                (unsigned long long)seq_from, (unsigned long long)seq_to);
        else
            jash_writef(w, ctx,
                "\033[1;32m✓ replay %llu..%llu: all entries deterministic\033[0m\n",
                (unsigned long long)seq_from, (unsigned long long)seq_to);
    }
    return non_det ? 1 : 0;
}

/* ── Dispatch ───────────────────────────────────────────────────────── */

static int sigil_dispatch(const jmod_call_t* call)
{
    if (!call->verb || strcmp(call->verb, "sigil") != 0) return -1;

    const char* sub = call->args ? call->args : "";
    while (*sub == ' ') sub++;

    if (strncmp(sub, "quarantine", 10) == 0 &&
        (sub[10] == '\0' || sub[10] == ' ')) {
        const char* qp = sub + 10;
        while (*qp == ' ') qp++;
        if (strncmp(qp, "status", 6) == 0 &&
            (qp[6] == '\0' || qp[6] == ' '))
            return cmd_quarantine_status(call->writer, call->writer_ctx);
        if (strncmp(qp, "apply", 5) == 0 &&
            (qp[5] == '\0' || qp[5] == ' '))
            return cmd_quarantine_apply(call->writer, call->writer_ctx);
        return cmd_quarantine_status(call->writer, call->writer_ctx);
    }

    /* S3: `sigil ratchet rekey|adopt` — the two honest exits from a shared
     * lineage. No default: the operator must say which tradeoff they are
     * taking, because the two differ in what the ledger can then claim. */
    if (strncmp(sub, "ratchet", 7) == 0 && (sub[7] == '\0' || sub[7] == ' ')) {
        const char* rp = sub + 7;
        while (*rp == ' ') rp++;
        if (strncmp(rp, "rekey", 5) == 0 && (rp[5] == '\0' || rp[5] == ' '))
            return sigil_ratchet_transition(call->writer, call->writer_ctx, 0);
        if (strncmp(rp, "adopt", 5) == 0 && (rp[5] == '\0' || rp[5] == ' '))
            return sigil_ratchet_transition(call->writer, call->writer_ctx, 1);
        jash_writes(call->writer, call->writer_ctx,
            "usage: sigil ratchet <rekey|adopt>\n"
            "  rekey  retire the shared lineage, fresh per-repo key (exclusive)\n"
            "  adopt  derive a per-repo key from the shared one (continuity,\n"
            "         weaker exclusivity — recorded in the sidecar)\n"
            "Both seal a boundary with the outgoing key. Neither rewrites or\n"
            "deletes anything, and neither promotes past entries to verified.\n");
        return 1;
    }

    /* JINV-054: `sigil list [--json] [N]` — a chain como dado consultavel. */
    if (strncmp(sub, "list", 4) == 0 && (sub[4] == '\0' || sub[4] == ' ')) {
        const char* lp = sub + 4;
        while (*lp == ' ') lp++;
        if (strncmp(lp, "--json", 6) == 0 && (lp[6] == '\0' || lp[6] == ' ')) {
            const char* np = lp + 6;
            while (*np == ' ') np++;
            long lim = *np ? atol(np) : 0;
            return cmd_chain_list_json(call->writer, call->writer_ctx, lim);
        }
        return cmd_chain_show(call->writer, call->writer_ctx);
    }

    /* JINV-053: `sigil verify [--json]` e alias de `sigil chain verify`.
     *
     * O JINV-051 entregou a saida de maquina sob `sigil chain verify --json`,
     * mas quem consome escreve `sigil verify --json` — e essa forma caia no
     * texto de ajuda. Uma feature entregue numa invocacao que o chamador nao
     * usa e uma feature nao entregue: mesma classe de "pronto no lugar
     * errado" que deixou o binario sem instalar. */
    if (strncmp(sub, "verify", 6) == 0 &&
        (sub[6] == '\0' || sub[6] == ' ')) {
        const char* vp = sub + 6;
        while (*vp == ' ') vp++;
        if (strncmp(vp, "--json", 6) == 0 && (vp[6] == '\0' || vp[6] == ' '))
            return sigil_chain_verify_result(
                cmd_chain_verify_json(call->writer, call->writer_ctx));
        return sigil_chain_verify_result(
            cmd_chain_verify(call->writer, call->writer_ctx));
    }

    if (strncmp(sub, "chain", 5) == 0) {
        const char* rest = sub + 5;
        while (*rest == ' ') rest++;
        if (strncmp(rest, "verify", 6) == 0 || strncmp(rest, "check", 5) == 0) {
            const char* rp = rest + (rest[0]=='v' ? 6 : 5);
            while (*rp == ' ') rp++;
            if (*rp) {
                /* JINV-051: saida de maquina. */
                if (strncmp(rp, "--json", 6) == 0 &&
                    (rp[6] == '\0' || rp[6] == ' '))
                    return sigil_chain_verify_result(
                        cmd_chain_verify_json(call->writer, call->writer_ctx));
                uint64_t a = 0, b = 0;
                if (sscanf(rp, "%llu..%llu", (unsigned long long*)&a,
                           (unsigned long long*)&b) == 2)
                    return sigil_chain_verify_result(cmd_chain_verify_range(
                        call->writer, call->writer_ctx, a, b, 1));
            }
            return sigil_chain_verify_result(
                cmd_chain_verify(call->writer, call->writer_ctx));
        }
        { const char* cp = rest;
          if (strncmp(cp, "--json", 6) == 0 && (cp[6] == '\0' || cp[6] == ' ')) {
              const char* np = cp + 6;
              while (*np == ' ') np++;
              return cmd_chain_list_json(call->writer, call->writer_ctx,
                                         *np ? atol(np) : 0);
          } }
        return cmd_chain_show(call->writer, call->writer_ctx);
    }

    if (strncmp(sub, "proof", 5) == 0) {
        const char* rest = sub + 5;
        while (*rest == ' ') rest++;
        if (*rest == '\0')
            return cmd_proof_emit(call->writer, call->writer_ctx, "");
        if (strncmp(rest, "--help", 6) == 0 && (rest[6] == '\0' || rest[6] == ' ')) {
            cmd_proof_usage(call->writer, call->writer_ctx);
            return 0;
        }
        if (strncmp(rest, "-h", 2) == 0 && (rest[2] == '\0' || rest[2] == ' ')) {
            cmd_proof_usage(call->writer, call->writer_ctx);
            return 0;
        }
        if (strncmp(rest, "emit", 4) == 0 && (rest[4] == '\0' || rest[4] == ' ')) {
            const char* out = rest + 4; while (*out == ' ') out++;
            return cmd_proof_emit(call->writer, call->writer_ctx, out);
        }
        if (strncmp(rest, "dsse", 4) == 0 && (rest[4] == '\0' || rest[4] == ' ')) {     /* W4-intoto: DSSE envelope */
            const char* out = rest + 4; while (*out == ' ') out++;
            return cmd_proof_dsse(call->writer, call->writer_ctx, out);
        }
        if (strncmp(rest, "identity", 8) == 0 &&
            (rest[8] == '\0' || rest[8] == ' '))
            return cmd_proof_identity(call->writer, call->writer_ctx);
        if (strncmp(rest, "verify", 6) == 0 && (rest[6] == '\0' || rest[6] == ' ')) {
            const char* in = rest + 6; while (*in == ' ') in++;
            return cmd_proof_verify(call->writer, call->writer_ctx, in);
        }
        if (strncmp(rest, "show", 4) == 0 && (rest[4] == '\0' || rest[4] == ' ')) {
            const char* in = rest + 4; while (*in == ' ') in++;
            return cmd_proof_show(call->writer, call->writer_ctx, in);
        }
        jash_writef(call->writer, call->writer_ctx,
                    "sigil proof: unknown subcommand '%s'\n", rest);
        return 1;
    }

    if (strncmp(sub, "history", 7) == 0 && (sub[7]==' ' || sub[7]=='\0')) {
        const char* rest = sub + 7; while (*rest == ' ') rest++;
        return cmd_log(call->writer, call->writer_ctx, rest);
    }

    if (strncmp(sub, "blame", 5) == 0) {
        const char* rest = sub + 5; while (*rest == ' ') rest++;
        return cmd_blame(call->writer, call->writer_ctx, rest);
    }

    /* S3 git-like UX over the chain. NB: `sigil log` is NOT here — the core
     * `sigil <off|log|sign|seal>` builtin owns that word (level set); the chain
     * view is `sigil history`. */
    if (strncmp(sub, "diff", 4) == 0 && (sub[4]==' ' || sub[4]=='\0')) {
        const char* rest = sub + 4; while (*rest == ' ') rest++;
        return cmd_diff(call->writer, call->writer_ctx, rest);
    }

    if (strncmp(sub, "bisect", 6) == 0 && (sub[6]==' ' || sub[6]=='\0')) {
        const char* rest = sub + 6; while (*rest == ' ') rest++;
        return cmd_bisect(call->writer, call->writer_ctx, rest);
    }

    if (strcmp(sub, "caps") == 0)
        return cmd_caps(call->writer, call->writer_ctx);

    if (strncmp(sub, "export", 6) == 0 && (sub[6]==' '||sub[6]=='\0')) {
        const char* rest = sub+6; while(*rest==' ') rest++;
        return cmd_export(call->writer, call->writer_ctx, rest);
    }

    if (strncmp(sub, "push", 4) == 0 && (sub[4]==' '||sub[4]=='\0')) {
        const char* rest = sub+4; while(*rest==' ') rest++;
        return cmd_push(call->writer, call->writer_ctx, rest);
    }

    if (strncmp(sub, "restore", 7) == 0 && (sub[7]==' ' || sub[7]=='\0')) {
        const char* rest = sub + 7; while (*rest == ' ') rest++;
        return cmd_restore(call->writer, call->writer_ctx, rest);
    }

    /* replay [--exec] <seq|range> — S9: determinism verification */
    if (strncmp(sub, "replay", 6) == 0 && (sub[6]==' ' || sub[6]=='\0')) {
        const char* rest = sub + 6; while (*rest == ' ') rest++;
        return cmd_replay(call->writer, call->writer_ctx, rest);
    }

    /* record <label> — append an event to the chain (used by undo module for tx attestation) */
    if (strncmp(sub, "record", 6) == 0 && (sub[6]==' ' || sub[6]=='\0')) {
        const char* rest = sub + 6; while (*rest == ' ') rest++;
        if (*rest) { chain_append(rest, 0); return 0; }
        return 1;
    }

    return -1;
}

/* Native-v1 extends the module through an optional symbol, without changing
 * the stable jmod_ops_t ABI or its legacy text/JSON dispatch. */
#include "sigil_native.inc"

/* ── Module ops ─────────────────────────────────────────────────────── */

jmod_ops_t jmod_ops = {
    .init          = sigil_init,
    .shutdown      = sigil_shutdown,
    .dispatch      = sigil_dispatch,
    .hook_pre_cmd  = NULL,
    .hook_post_cmd = sigil_post_cmd,
    .hook_exit     = NULL,
    .name          = "sigil",
    .version       = "0.7.0",
    .description   = "Verifiable execution chain — BLAKE3 + ratchet + pledge + replay (S1-S9)",
    .caps_used     = JMOD_CAP_FS_READ | JMOD_CAP_FS_WRITE |
                     JMOD_CAP_EXEC |
                     JMOD_CAP_STATE_READ | JMOD_CAP_CRYPTO |
                     JMOD_CAP_HOOK_POST_CMD,
};
