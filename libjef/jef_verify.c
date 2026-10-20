// SPDX-License-Identifier: LicenseRef-SPL-1.0
// libjef — independent verifier for jash attestation artifacts (see jef_verify.h).
//
// Byte-for-byte compatible with the producer in modules/sigil/sigil_module.c:
//   chain line : "<seq> <sec>.<nsec> <blake3-hex> tag=<hex> rc=<int> cmd=<text>"
//   chain hash : hₙ = BLAKE3(hₙ₋₁ ‖ ts_ns(8B BE) ‖ rc(4B LE) ‖ cmd)
//   proof body : jashproof/1 text, signed (ed25519) over the body, or wrapped
//                in a DSSE envelope signed over the DSSE PAE.

#include "jef_verify.h"
#include "jef.h"          /* jash_blake3_chain, jash_blake3_hex, ed25519_verify */
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

/* ── small self-contained codecs ──────────────────────────────────────── */

static int v_hexdec(const char* in, size_t length, uint8_t* out, unsigned n) {
    if (length != (size_t)n * 2) return -1;
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
static int v_b64val(char c){
    if (c>='A'&&c<='Z') { return c-'A'; }
    if (c>='a'&&c<='z') { return c-'a'+26; }
    if (c>='0'&&c<='9') { return c-'0'+52; }
    if (c=='+') { return 62; }
    if (c=='/') { return 63; }
    return -1;
}
static int v_b64dec(const char* in, uint8_t* out, size_t cap){
    size_t length = strlen(in), used = 0;
    if (length % 4) return -1;
    for (size_t i = 0; i < length; i += 4) {
        int a = v_b64val(in[i]), b = v_b64val(in[i + 1]);
        int c = in[i + 2] == '=' ? -2 : v_b64val(in[i + 2]);
        int d = in[i + 3] == '=' ? -2 : v_b64val(in[i + 3]);
        if (a < 0 || b < 0 || c == -1 || d == -1) return -1;
        if (c == -2) {
            if (d != -2 || i + 4 != length || (b & 15) || used + 1 > cap)
                return -1;
            out[used++] = (uint8_t)((a << 2) | (b >> 4));
        } else if (d == -2) {
            if (i + 4 != length || (c & 3) || used + 2 > cap) return -1;
            out[used++] = (uint8_t)((a << 2) | (b >> 4));
            out[used++] = (uint8_t)((b << 4) | (c >> 2));
        } else {
            if (used + 3 > cap) return -1;
            out[used++] = (uint8_t)((a << 2) | (b >> 4));
            out[used++] = (uint8_t)((b << 4) | (c >> 2));
            out[used++] = (uint8_t)((c << 6) | d);
        }
    }
    return (int)used;
}
static int v_pae(const char* type, const char* body, size_t blen, uint8_t* out, size_t cap){
    int n = snprintf((char*)out, cap, "DSSEv1 %zu %s %zu ", strlen(type), type, blen);
    if (n<0 || (size_t)n+blen>cap) return -1;
    memcpy(out+n, body, blen);
    return n+(int)blen;
}

/* ── chain verification ───────────────────────────────────────────────── */

/* Canonical producer decimal, with no sign, padding, suffix, or overflow. */
static int v_uint(const char* p, size_t n, unsigned long long limit,
                   unsigned long long* out) {
    if (!n || (n > 1 && p[0] == '0')) return -1;
    unsigned long long value = 0;
    for (size_t i = 0; i < n; i++) {
        if (p[i] < '0' || p[i] > '9') return -1;
        unsigned digit = (unsigned)(p[i] - '0');
        if (value > limit / 10 || (value == limit / 10 && digit > limit % 10))
            return -1;
        value = value * 10 + digit;
    }
    *out = value;
    return 0;
}

int jef_verify_chain_stream(FILE* f, jef_chain_state_t* state) {
    if (state) memset(state, 0, sizeof *state);
    if (!f || !state) return -1;

    /* Includes LF; room for the producer's 4083 command bytes plus metadata. */
    char line[16384];
    uint8_t running[32] = {0};
    unsigned long long checked = 0;
    unsigned long long commands = 0;
    int result = 1;

    for (;;) {
        size_t used = 0;
        int ch;
        while ((ch = fgetc(f)) != EOF && ch != '\n') {
            if (ch == 0 || used == sizeof line - 1) goto done;
            line[used++] = (char)ch;
        }
        if (ferror(f)) { result = -1; goto done; }
        if (ch == EOF) {
            if (used) goto done; /* Unterminated physical record. */
            result = 0;
            goto done;
        }
        line[used] = '\0';

        unsigned long long seq, sec, nsec = 0;
        const char* p = line;
        const char* end = strchr(p, ' ');
        if (!end || v_uint(p, (size_t)(end - p), ULLONG_MAX, &seq) ||
            seq != checked) goto done;
        p = end + 1;
        end = strchr(p, ' ');
        if (!end) goto done;
        const char* dot = memchr(p, '.', (size_t)(end - p));
        if (!dot || end - dot != 10 ||
            v_uint(p, (size_t)(dot - p), UINT64_MAX / 1000000000ULL, &sec))
            goto done;
        for (const char* q = dot + 1; q < end; q++) {
            if (*q < '0' || *q > '9') goto done;
            nsec = nsec * 10 + (unsigned)(*q - '0');
        }
        if (sec > (UINT64_MAX - nsec) / 1000000000ULL) goto done;
        uint64_t ts_ns = sec * 1000000000ULL + nsec;

        p = end + 1;
        end = strchr(p, ' ');
        uint8_t hash[32], tag[32];
        if (!end || v_hexdec(p, (size_t)(end - p), hash, 32)) goto done;
        p = end + 1;
        uint32_t rc = 0;
        int have_rc = 0, have_tag = 0;
        /* Only header tokens qualify; command text is never field lookup. */
        while (strncmp(p, "cmd=", 4) != 0) {
            end = strchr(p, ' ');
            if (!end) goto done;
            size_t name = strspn(p, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-");
            if (!name || p + name >= end || p[name] != '=') goto done;
            const char* value = p + name + 1;
            size_t length = (size_t)(end - value);
            if (name == 2 && memcmp(p, "rc", 2) == 0) {
                int negative = length && *value == '-';
                unsigned long long magnitude;
                if (have_rc || v_uint(value + negative, length - negative,
                        (unsigned long long)INT_MAX + negative, &magnitude) ||
                    (negative && !magnitude)) goto done;
                rc = negative ? 0u - (uint32_t)magnitude : (uint32_t)magnitude;
                have_rc = 1;
            } else if (name == 3 && memcmp(p, "tag", 3) == 0) {
                if (have_tag || v_hexdec(value, length, tag, 32)) goto done;
                have_tag = 1; /* Syntax only, not a keyed-tag verification. */
            }
            p = end + 1;
        }
        if (!have_rc || !have_tag) goto done;
        const char* cmd = p + 4;
        size_t clen = (size_t)(line + used - cmd);
        if (clen > 4083) goto done;

        uint8_t payload[4096]; size_t plen = 0;
        for (int i=7;i>=0;i--) payload[plen++]=(uint8_t)(ts_ns>>(i*8));
        payload[plen++]=(uint8_t)(rc&0xFF);       payload[plen++]=(uint8_t)((rc>>8)&0xFF);
        payload[plen++]=(uint8_t)((rc>>16)&0xFF); payload[plen++]=(uint8_t)((rc>>24)&0xFF);
        if (clen) { memcpy(payload+plen, cmd, clen); plen += clen; }

        uint8_t expect[32]; jash_blake3_chain(running, payload, plen, expect);
        if (memcmp(expect, hash, 32) != 0 || checked == ULLONG_MAX) goto done;
        memcpy(running, expect, 32);
        checked++;
        /* SPEC jashproof/1 counts non-sentinel commands; all records above
         * still participate in sequence validation and the hash chain. */
        if (strncmp(cmd, "SESSION_END", 11) != 0) commands++;
    }
done:
    if (ferror(f)) result = -1;
    if (result == 0) {
        state->records = checked;
        state->commands = commands;
        memcpy(state->tip, running, sizeof state->tip);
    }
    return result;
}

int jef_verify_chain(const char* chain_path, unsigned long long* out_entries) {
    if (out_entries) *out_entries = 0;
    if (!chain_path) return -1;
    FILE* f = fopen(chain_path, "rb");
    if (!f) return -1;
    jef_chain_state_t state;
    int result = jef_verify_chain_stream(f, &state);
    if (fclose(f) != 0) result = -1;
    if (out_entries && result == 0) *out_entries = state.records;
    return result;
}

/* ── proof verification ───────────────────────────────────────────────── */

#define V_DSSE_TYPE "application/vnd.jash.jashproof.v1+text"

typedef struct {
    char type[128], keyid[65];
    uint8_t payload[2049], pubkey[ED25519_PUBKEY], sig[ED25519_SIG];
    size_t payload_len;
} v_dsse;

static int v_dsse_decode(const char* json, size_t length, v_dsse* envelope);

static int verify_dsse(const char* buf, size_t length, v_dsse* envelope) {
    if (v_dsse_decode(buf, length, envelope) != 0) return 1;
    uint8_t pae[2560];
    int paelen = v_pae(envelope->type, (const char*)envelope->payload,
                      envelope->payload_len, pae, sizeof pae);
    if (paelen < 0) return 1;
    return ed25519_verify(envelope->sig, pae, (uint32_t)paelen,
                          envelope->pubkey) == 0 ? 0 : 1;
}

typedef struct {
    char keyid[65];
    uint8_t pubkey[ED25519_PUBKEY], sig[ED25519_SIG];
    size_t body_len;
} v_text_proof;

/* Structural parsing is shared by verification and identity display. Unknown
 * key: value extensions are signed bytes, not authorization claims. */
static int v_text_decode(const char* buf, size_t length, v_text_proof* proof) {
    static const char header[] = "jashproof/1\n";
    if (length < sizeof header - 1 ||
        memcmp(buf, header, sizeof header - 1) != 0) return 1;
    const char* p = buf + sizeof header - 1;
    const char* limit = buf + length;
    unsigned seen = 0;
    while (p < limit) {
        const char* lf = memchr(p, '\n', (size_t)(limit - p));
        const char* end = lf ? lf : limit;
        const char* colon = memchr(p, ':', (size_t)(end - p));
        if (!colon || colon == p || end - colon < 2 || colon[1] != ' ') return 1;
        size_t name = (size_t)(colon - p);
        for (size_t i = 0; i < name; i++) {
            char c = p[i];
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '-')) return 1;
        }
        const char* value = colon + 2;
        size_t n = (size_t)(end - value);
        unsigned field = 0;
        if (name == 3 && memcmp(p, "sig", 3) == 0) {
            /* Producer emits LF; accept its omission, but no other tail. */
            if (seen != 15 || (lf && lf + 1 != limit) ||
                v_hexdec(value, n, proof->sig, ED25519_SIG)) return 1;
            proof->body_len = (size_t)(p - buf);
            return 0;
        }
        if (!lf) return 1;
        if (name == 6 && memcmp(p, "pubkey", 6) == 0) {
            field = 1;
            if (v_hexdec(value, n, proof->pubkey, ED25519_PUBKEY)) return 1;
            memcpy(proof->keyid, value, 64);
            proof->keyid[64] = '\0';
        } else if (name == 8 && memcmp(p, "sig_algo", 8) == 0) {
            field = 2;
            if (n != 7 || memcmp(value, "ed25519", 7) != 0) return 1;
        } else if (name == 13 && memcmp(p, "chain_entries", 13) == 0) {
            field = 4;
            unsigned long long entries;
            if (v_uint(value, n, ULLONG_MAX, &entries)) return 1;
        } else if (name == 9 && memcmp(p, "chain_tip", 9) == 0) {
            field = 8;
            uint8_t tip[32];
            if (v_hexdec(value, n, tip, 32)) return 1;
        }
        if (seen & field) return 1;
        seen |= field;
        p = lf + 1;
    }
    return 1;
}

static int v_read_proof(const char* proof_path, char buf[8192], size_t* length) {
    if (!proof_path) return -1;
    FILE* f = fopen(proof_path, "rb");
    if (!f) return -1;
    size_t n = fread(buf, 1, 8191, f);
    int extra = fgetc(f), error = ferror(f);
    int close_error = fclose(f);
    buf[n] = '\0';
    if (error || close_error != 0) return -1;
    if (extra != EOF || memchr(buf, '\0', n)) return 1;
    *length = n;
    return 0;
}

static int v_verify_proof_data(const char* buf, size_t n,
                               uint8_t out_pubkey[ED25519_PUBKEY]) {
    const char* s = buf; while (*s==' '||*s=='\n'||*s=='\t'||*s=='\r') s++;
    if (*s == '{') {
        v_dsse envelope;
        int result = verify_dsse(buf, n, &envelope);
        if (!result && out_pubkey) memcpy(out_pubkey, envelope.pubkey, ED25519_PUBKEY);
        return result;
    }

    v_text_proof proof;
    if (v_text_decode(buf, n, &proof)) return 1;
    if (ed25519_verify(proof.sig, (const uint8_t*)buf,
                       (uint32_t)proof.body_len, proof.pubkey) != 0) return 1;
    if (out_pubkey) memcpy(out_pubkey, proof.pubkey, ED25519_PUBKEY);
    return 0;
}

int jef_verify_proof(const char* proof_path) {
    char buf[8192];
    size_t n;
    int result = v_read_proof(proof_path, buf, &n);
    return result ? result : v_verify_proof_data(buf, n, NULL);
}


/* ── JINV-024: verificacao ancorada numa identidade ──────────────────────── */

int jef_proof_pubkey_hex(const char* proof_path, char* out_hex, unsigned cap) {
    if (!proof_path || !out_hex || cap < 65) return -1;
    char buf[8192];
    size_t n;
    int result = v_read_proof(proof_path, buf, &n);
    if (result) return result;

    const char* s = buf + strspn(buf, " \t\r\n");
    if (*s == '{') {
        v_dsse envelope;
        if (v_dsse_decode(buf, n, &envelope) != 0) return 1;
        memcpy(out_hex, envelope.keyid, sizeof envelope.keyid);
        return 0;
    }

    v_text_proof proof;
    if (v_text_decode(buf, n, &proof)) return 1;
    memcpy(out_hex, proof.keyid, sizeof proof.keyid);
    return 0;
}

int jef_verify_proof_pinned(const char* proof_path,
                            const char* expected_pubkey_hex) {
    uint8_t expected[ED25519_PUBKEY];
    if (!expected_pubkey_hex ||
        v_hexdec(expected_pubkey_hex, strnlen(expected_pubkey_hex, 65),
                 expected, ED25519_PUBKEY)) return -2;

    char buf[8192];
    size_t n;
    int result = v_read_proof(proof_path, buf, &n);
    if (result) return result;
    uint8_t got[ED25519_PUBKEY];
    result = v_verify_proof_data(buf, n, got);
    if (result) return result;
    unsigned difference = 0;
    for (unsigned i = 0; i < ED25519_PUBKEY; i++) difference |= got[i] ^ expected[i];
    return difference ? 1 : 0;
}

int jef_verify_proof_bytes_pinned(const char* bytes, unsigned length,
                                  const char* expected_pubkey_hex) {
    uint8_t expected[ED25519_PUBKEY];
    if (!expected_pubkey_hex || v_hexdec(expected_pubkey_hex, strnlen(expected_pubkey_hex, 65), expected, ED25519_PUBKEY)) return -2;
    if (!bytes || !length || length >= 8192 || memchr(bytes, 0, length)) return 1;
    char snapshot[8192]; memcpy(snapshot, bytes, length); snapshot[length] = 0;
    uint8_t got[ED25519_PUBKEY];
    int rc = v_verify_proof_data(snapshot, length, got);
    if (rc) return rc;
    unsigned difference = 0;
    for (unsigned i = 0; i < ED25519_PUBKEY; i++) difference |= got[i] ^ expected[i];
    return difference ? 1 : 0;
}

int jef_proof_inspect_bytes(const char* bytes, unsigned length,
                             jef_proof_observation_t* observation) {
    if (!observation) return -1;
    memset(observation, 0, sizeof *observation);
    if (!bytes) return -1;
    if (!length || length >= 8192 || memchr(bytes, 0, length)) return 1;
    char snapshot[8192]; memcpy(snapshot, bytes, length); snapshot[length] = 0;
    const char* first = snapshot + strspn(snapshot, " \t\r\n");
    if (*first == '{') {
        v_dsse envelope;
        if (v_dsse_decode(snapshot, length, &envelope)) return 1;
        observation->dsse = 1;
        memcpy(observation->public_key, envelope.keyid, sizeof envelope.keyid);
        memcpy(observation->payload_type, envelope.type, sizeof envelope.type);
        observation->payload_length = (unsigned)envelope.payload_len;
        memcpy(observation->payload, envelope.payload, envelope.payload_len);
        uint8_t pae[2560];
        int size = v_pae(envelope.type, (const char*)envelope.payload,
                         envelope.payload_len, pae, sizeof pae);
        observation->signature_valid = size >= 0 &&
            ed25519_verify(envelope.sig, pae, (uint32_t)size, envelope.pubkey) == 0;
    } else {
        v_text_proof proof;
        if (v_text_decode(snapshot, length, &proof)) return 1;
        memcpy(observation->public_key, proof.keyid, sizeof proof.keyid);
        observation->payload_length = (unsigned)proof.body_len;
        memcpy(observation->payload, snapshot, proof.body_len);
        observation->signature_valid = ed25519_verify(proof.sig,
            (const uint8_t*)snapshot, (uint32_t)proof.body_len, proof.pubkey) == 0;
    }
    return 0;
}

static char* v_read_alloc(const char* path, size_t limit, size_t* out_len) {
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
        (uintmax_t)st.st_size > limit) { close(fd); return NULL; }
    FILE* f = fdopen(fd, "rb");
    if (!f) { close(fd); return NULL; }
    size_t n = (size_t)st.st_size;
    char* data = (char*)malloc(n + 1);
    if (!data) { fclose(f); return NULL; }
    size_t got = fread(data, 1, n, f);
    int extra = fgetc(f);
    int error = ferror(f);
    fclose(f);
    if (got != n || extra != EOF || error) { free(data); return NULL; }
    data[got] = '\0';
    if (out_len) *out_len = got;
    return data;
}

static void v_json_ws(const char** cursor) {
    *cursor += strspn(*cursor, " \t\r\n");
}

/* One string decoder for validation and field lookup. Decode escaped names
 * before comparing them, so an escaped duplicate cannot hide from lookup. */
static int v_json_text(const char** cursor, char* out, size_t cap) {
    const unsigned char* p = (const unsigned char*)*cursor;
    if (*p++ != '"') return -1;
    size_t used = 0;
    while (*p && *p != '"') {
        unsigned cp = *p++;
        if (cp < 0x20) return -1;
        if (cp == '\\') {
            unsigned escape = *p++;
            switch (escape) {
                case '"': case '\\': case '/': cp = escape; break;
                case 'b': cp = '\b'; break;
                case 'f': cp = '\f'; break;
                case 'n': cp = '\n'; break;
                case 'r': cp = '\r'; break;
                case 't': cp = '\t'; break;
                case 'u': {
                    uint8_t hex[2];
                    if (strnlen((const char*)p, 4) < 4 ||
                        v_hexdec((const char*)p, 4, hex, 2) != 0) return -1;
                    cp = ((unsigned)hex[0] << 8) | hex[1];
                    p += 4;
                    if (cp >= 0xd800 && cp <= 0xdbff) {
                        if (strnlen((const char*)p, 6) < 6 || p[0] != '\\' ||
                            p[1] != 'u' || v_hexdec((const char*)p + 2, 4, hex, 2) != 0)
                            return -1;
                        unsigned low = ((unsigned)hex[0] << 8) | hex[1];
                        if (low < 0xdc00 || low > 0xdfff) return -1;
                        cp = 0x10000 + ((cp - 0xd800) << 10) + low - 0xdc00;
                        p += 6;
                    } else if (cp >= 0xdc00 && cp <= 0xdfff) return -1;
                    break;
                }
                default: return -1;
            }
        } else if (cp >= 0x80) {
            unsigned count, min;
            if (cp >= 0xc2 && cp <= 0xdf) { count = 1; min = 0x80; cp &= 31; }
            else if (cp >= 0xe0 && cp <= 0xef) { count = 2; min = 0x800; cp &= 15; }
            else if (cp >= 0xf0 && cp <= 0xf4) { count = 3; min = 0x10000; cp &= 7; }
            else return -1;
            for (unsigned i = 0; i < count; i++) {
                if ((*p & 0xc0) != 0x80) return -1;
                cp = (cp << 6) | (*p++ & 63);
            }
            if (cp < min || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
                return -1;
        }
        unsigned char bytes[4];
        size_t n = 0;
        if (cp < 0x80) bytes[n++] = (unsigned char)cp;
        else {
            if (cp >= 0x10000) bytes[n++] = (unsigned char)(0xf0 | (cp >> 18));
            else if (cp >= 0x800) bytes[n++] = (unsigned char)(0xe0 | (cp >> 12));
            else bytes[n++] = (unsigned char)(0xc0 | (cp >> 6));
            if (cp >= 0x10000) bytes[n++] = (unsigned char)(0x80 | ((cp >> 12) & 63));
            if (cp >= 0x800) bytes[n++] = (unsigned char)(0x80 | ((cp >> 6) & 63));
            bytes[n++] = (unsigned char)(0x80 | (cp & 63));
        }
        if (out) {
            if (cp == 0 || used + n >= cap) return -1;
            memcpy(out + used, bytes, n);
        }
        used += n;
    }
    if (*p != '"' || (out && used >= cap)) return -1;
    if (out) out[used] = '\0';
    *cursor = (const char*)p + 1;
    return 0;
}

static int v_json_value(const char** cursor, unsigned depth) {
    if (depth > 64) return -1;
    v_json_ws(cursor);
    const char* p = *cursor;
    if (*p == '"') return v_json_text(cursor, NULL, 0);
    if (*p == '{' || *p == '[') {
        int object = *p == '{';
        char close = object ? '}' : ']';
        p++;
        v_json_ws(&p);
        if (*p != close) for (;;) {
            if (object) {
                if (v_json_text(&p, NULL, 0) != 0) return -1;
                v_json_ws(&p);
                if (*p++ != ':') return -1;
            }
            if (v_json_value(&p, depth + 1) != 0) return -1;
            v_json_ws(&p);
            if (*p != ',') break;
            p++;
            v_json_ws(&p);
        }
        if (*p++ != close) return -1;
    } else if (strncmp(p, "true", 4) == 0 || strncmp(p, "null", 4) == 0) p += 4;
    else if (strncmp(p, "false", 5) == 0) p += 5;
    else {
        if (*p == '-') p++;
        if (*p == '0') p++;
        else {
            if (*p < '1' || *p > '9') return -1;
            p += strspn(p, "0123456789");
        }
        if (*p == '.') {
            p++;
            size_t n = strspn(p, "0123456789");
            if (!n) return -1;
            p += n;
        }
        if (*p == 'e' || *p == 'E') {
            p++;
            if (*p == '+' || *p == '-') p++;
            size_t n = strspn(p, "0123456789");
            if (!n) return -1;
            p += n;
        }
    }
    *cursor = p;
    return 0;
}

static int v_json_object_exact(const char* data, size_t length) {
    if (!data || !length || memchr(data, '\0', length)) return 0;
    const char* p = data;
    v_json_ws(&p);
    int ok = *p == '{' && v_json_value(&p, 0) == 0;
    v_json_ws(&p);
    return ok && p == data + length;
}

/* Select unique members of one object. Closed schemas additionally reject
 * unknown members and require every named field (used for DSSE envelopes). */
static int v_json_members(const char** cursor, const char* const* keys,
                          const char** values, size_t count, int closed) {
    const char* p = *cursor;
    for (size_t i = 0; i < count; i++) values[i] = NULL;
    v_json_ws(&p);
    if (*p++ != '{') return -1;
    v_json_ws(&p);
    if (*p != '}') for (;;) {
        char name[256];
        if (v_json_text(&p, name, sizeof name) != 0) return -1;
        v_json_ws(&p);
        if (*p++ != ':') return -1;
        v_json_ws(&p);
        size_t i;
        for (i = 0; i < count; i++) {
            size_t key_len = strlen(keys[i]);
            if (key_len >= 2 && strlen(name) == key_len - 2 &&
                memcmp(name, keys[i] + 1, key_len - 2) == 0) break;
        }
        if (i < count) {
            if (values[i]) return -1;
            values[i] = p;
        } else if (closed) return -1;
        if (v_json_value(&p, 1) != 0) return -1;
        v_json_ws(&p);
        if (*p != ',') break;
        p++;
        v_json_ws(&p);
    }
    if (*p++ != '}') return -1;
    if (closed) for (size_t i = 0; i < count; i++)
        if (!values[i]) return -1;
    v_json_ws(&p);
    *cursor = p;
    return 0;
}

/* Only top-level members qualify, never nested fields or string contents. */
static const char* v_json_member(const char* json, const char* key) {
    const char* found;
    if (v_json_members(&json, &key, &found, 1, 0) != 0 || *json != '\0')
        return NULL;
    return found;
}

static int v_json_string(const char* json, const char* key,
                         char* out, size_t cap) {
    const char* p = v_json_member(json, key);
    return p ? v_json_text(&p, out, cap) : -1;
}

/* Every consumer uses these exact decoded bytes, including the signature
 * verifier. No second interpretation of the envelope is permitted. */
static int v_dsse_decode(const char* json, size_t length, v_dsse* envelope) {
    static const char* const keys[] = {
        "\"payloadType\"", "\"payload\"", "\"signatures\"",
    };
    static const char* const signature_keys[] = {"\"keyid\"", "\"sig\""};
    const char* values[3];
    const char* signature_values[2];
    const char* p = json;
    if (!v_json_object_exact(json, length) ||
        v_json_members(&p, keys, values, 3, 1) != 0 || *p != '\0') return 1;
    char encoded[4096], signature[128];
    if (v_json_text(&values[0], envelope->type, sizeof envelope->type) != 0 ||
        v_json_text(&values[1], encoded, sizeof encoded) != 0) return 1;
    p = values[2];
    if (*p++ != '[' ||
        v_json_members(&p, signature_keys, signature_values, 2, 1) != 0 ||
        *p != ']') return 1;
    if (v_json_text(&signature_values[0], envelope->keyid,
                    sizeof envelope->keyid) != 0 ||
        strlen(envelope->keyid) != 64 ||
        v_hexdec(envelope->keyid, strlen(envelope->keyid),
                 envelope->pubkey, ED25519_PUBKEY) != 0 ||
        v_json_text(&signature_values[1], signature, sizeof signature) != 0 ||
        v_b64dec(signature, envelope->sig, sizeof envelope->sig) != ED25519_SIG)
        return 1;
    int n = v_b64dec(encoded, envelope->payload, sizeof envelope->payload - 1);
    if (n < 0) return 1;
    envelope->payload_len = (size_t)n;
    envelope->payload[n] = '\0';
    return 0;
}

static int v_json_size(const char* json, const char* key,
                       unsigned long long* out) {
    const char* p = v_json_member(json, key);
    if (!p) return -1;
    if (*p < '0' || *p > '9') return -1;
    errno = 0;
    char* end = NULL;
    unsigned long long value = strtoull(p, &end, 10);
    if (errno || end == p) return -1;
    const char* after = end;
    v_json_ws(&after);
    if (*after != ',' && *after != '}') return -1;
    *out = value;
    return 0;
}

static int v_hash_regular_file(const char* path, unsigned long long expected_size,
                               char out[65]) {
    int flags = O_RDONLY | O_NONBLOCK;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    int fd = open(path, flags);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
        (unsigned long long)st.st_size != expected_size) {
        close(fd);
        return -1;
    }
    struct blake3 hash;
    blake3_init(&hash);
    uint8_t buf[8192];
    unsigned long long total = 0;
    while (total < expected_size) {
        size_t want = sizeof buf;
        unsigned long long remaining = expected_size - total;
        if (remaining < want) want = (size_t)remaining;
        ssize_t n = read(fd, buf, want);
        if (n > 0) {
            blake3_update(&hash, buf, (uint64_t)n);
            total += (unsigned long long)n;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        close(fd);
        return -1;
    }
    ssize_t extra;
    do { extra = read(fd, buf, 1); } while (extra < 0 && errno == EINTR);
    close(fd);
    if (extra != 0) return -1;
    uint8_t digest[32];
    blake3_final(&hash, digest, sizeof digest);
    jash_blake3_hex(digest, out);
    return 0;
}

static int v_hash_regular_fd(int fd, unsigned long long expected_size,
                             char out[65]) {
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
        (unsigned long long)st.st_size != expected_size)
        return -1;
    struct blake3 hash;
    blake3_init(&hash);
    uint8_t buf[8192];
    unsigned long long total = 0;
    while (total < expected_size) {
        size_t want = sizeof buf;
        unsigned long long remaining = expected_size - total;
        if (remaining < want) want = (size_t)remaining;
        ssize_t n = pread(fd, buf, want, (off_t)total);
        if (n > 0) {
            blake3_update(&hash, buf, (uint64_t)n);
            total += (unsigned long long)n;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        return -1;
    }
    uint8_t digest[32];
    blake3_final(&hash, digest, sizeof digest);
    jash_blake3_hex(digest, out);
    return 0;
}

int jef_verify_runtime_receipt_with_streams(
                               const char* receipt_path,
                               const char* response_path,
                               const char* request_path,
                               const char* expected_pubkey_hex,
                               const char* expected_receipt_ref,
                               int stdout_fd,
                               int stderr_fd) {
    size_t receipt_len = 0, response_len = 0, request_len = 0;
    char* receipt = v_read_alloc(receipt_path, 8192, &receipt_len);
    char* response = v_read_alloc(response_path, 600 * 1024, &response_len);
    char* request = v_read_alloc(request_path, 32 * 1024, &request_len);
    if (!receipt || !response || !request) {
        free(receipt); free(response); free(request); return -1;
    }
    v_dsse envelope;
    if (!v_json_object_exact(receipt, receipt_len) ||
        !v_json_object_exact(response, response_len) ||
        !v_json_object_exact(request, request_len) ||
        !expected_pubkey_hex || strnlen(expected_pubkey_hex, 65) != 64 ||
        verify_dsse(receipt, receipt_len, &envelope) != 0 ||
        strcasecmp(envelope.keyid, expected_pubkey_hex) != 0) {
        free(receipt); free(response); free(request); return 1;
    }

    static const char runtime_type_v1[] =
        "application/vnd.jash.agent-runtime-receipt.v1+json";
    static const char runtime_type_v2[] =
        "application/vnd.jash.agent-runtime-receipt.v2+json";
    char expected_receipt[65], expected_result[65];
    char receipt_ref[256], response_pubkey[65];
    const char* payload_type = envelope.type;
    if (v_json_string(response, "\"receipt_blake3\"", expected_receipt,
                      sizeof expected_receipt) != 0 ||
        v_json_string(response, "\"receipt_ref\"", receipt_ref,
                      sizeof receipt_ref) != 0 ||
        v_json_string(response, "\"receipt_pubkey\"", response_pubkey,
                      sizeof response_pubkey) != 0) {
        free(receipt); free(response); free(request); return 1;
    }
    if ((strcmp(payload_type, runtime_type_v1) != 0 &&
         strcmp(payload_type, runtime_type_v2) != 0) ||
        strcasecmp(response_pubkey, expected_pubkey_hex) != 0) {
        free(receipt); free(response); free(request); return 1;
    }
    const char* receipt_name = expected_receipt_ref;
    if (!receipt_name) {
        receipt_name = strrchr(receipt_path, '/');
        receipt_name = receipt_name ? receipt_name + 1 : receipt_path;
    }
    if (strcmp(receipt_name, receipt_ref) != 0) {
        free(receipt); free(response); free(request); return 1;
    }
    uint8_t digest[32]; char got[65];
    jash_blake3(receipt, receipt_len, digest);
    jash_blake3_hex(digest, got);
    if (strcmp(got, expected_receipt) != 0) {
        free(receipt); free(response); free(request); return 1;
    }

    const char* payload = (const char*)envelope.payload;
    if (!v_json_object_exact(payload, envelope.payload_len)) {
        free(receipt); free(response); free(request); return 1;
    }
    char schema[96];
    if (v_json_string(payload, "\"schema\"", schema,
                      sizeof schema) != 0 ||
        (strcmp(schema, "jash.agent-runtime-receipt.v1") != 0 &&
         strcmp(schema, "jash.agent-runtime-receipt.v2") != 0)) {
        free(receipt); free(response); free(request); return 1;
    }
    if ((strcmp(schema, "jash.agent-runtime-receipt.v1") == 0 &&
         strcmp(payload_type, runtime_type_v1) != 0) ||
        (strcmp(schema, "jash.agent-runtime-receipt.v2") == 0 &&
         strcmp(payload_type, runtime_type_v2) != 0)) {
        free(receipt); free(response); free(request); return 1;
    }
    if (strcmp(schema, "jash.agent-runtime-receipt.v2") == 0) {
        char cwd_hex[1025];
        if (v_json_string((const char*)payload, "\"cwd_bytes_hex\"",
                          cwd_hex, sizeof cwd_hex) != 0 || !cwd_hex[0] ||
            (strlen(cwd_hex) & 1)) {
            free(receipt); free(response); free(request); return 1;
        }
        for (size_t i = 0; cwd_hex[i]; i++) {
            if (!((cwd_hex[i] >= '0' && cwd_hex[i] <= '9') ||
                  (cwd_hex[i] >= 'a' && cwd_hex[i] <= 'f'))) {
                free(receipt); free(response); free(request); return 1;
            }
        }
    }
    char request_hash[65], stdout_hash[65], stderr_hash[65];
    if (v_json_string((const char*)payload, "\"request_blake3\"",
                      request_hash, sizeof request_hash) != 0 ||
        v_json_string((const char*)payload, "\"stdout_blake3\"",
                      stdout_hash, sizeof stdout_hash) != 0 ||
        v_json_string((const char*)payload, "\"stderr_blake3\"",
                      stderr_hash, sizeof stderr_hash) != 0) {
        free(receipt); free(response); free(request); return 1;
    }
    /* The producer hashes the JSON frame body after consuming its JSONL LF. */
    if (request_len > 0 && request[request_len - 1] == '\n') request_len--;
    jash_blake3(request, request_len, digest);
    jash_blake3_hex(digest, got);
    if (strcmp(got, request_hash) != 0) {
        free(receipt); free(response); free(request); return 1;
    }
    char deterministic_ref[80];
    snprintf(deterministic_ref, sizeof deterministic_ref, "%s.dsse", request_hash);
    if (strcmp(receipt_ref, deterministic_ref) != 0) {
        free(receipt); free(response); free(request); return 1;
    }
    free(request);
    if (v_json_string((const char*)payload, "\"result_blake3\"",
                      expected_result, sizeof expected_result) != 0) {
        free(receipt); free(response); return 1;
    }

    const char* marker = strstr(response, ",\"receipt_ref\":");
    if (!marker) { free(receipt); free(response); return 1; }
    char expected_tail[640];
    int tn = snprintf(expected_tail, sizeof expected_tail,
        ",\"receipt_ref\":\"%s\",\"receipt_blake3\":\"%s\","
        "\"receipt_pubkey\":\"%s\"}\n",
        receipt_ref, expected_receipt, response_pubkey);
    if (tn <= 0 || (size_t)tn >= sizeof expected_tail ||
        strcmp(marker, expected_tail) != 0) {
        free(receipt); free(response); return 1;
    }

    const char* tags[2] = {"stdout", "stderr"};
    const char* stream_hashes[2] = {stdout_hash, stderr_hash};
    const int stream_fds[2] = {stdout_fd, stderr_fd};
    for (int i = 0; i < 2; i++) {
        char key[32], size_key[32], ref[256];
        snprintf(key, sizeof key, "\"%s_ref\"", tags[i]);
        if (v_json_string(response, key, ref, sizeof ref) != 0) {
            if (stream_fds[i] >= 0) {
                free(receipt); free(response); return 1;
            }
            continue;
        }
        if (!ref[0] || strstr(ref, "..") || strchr(ref, '/')) {
            free(receipt); free(response); return 1;
        }
        char overflow[1024];
        const char* stream_path = NULL;
        if (stream_fds[i] < 0) {
            snprintf(overflow, sizeof overflow, "%s", receipt_path);
            char* slash = strrchr(overflow, '/');
            if (!slash) { free(receipt); free(response); return 1; }
            *slash = '\0';
            slash = strrchr(overflow, '/');
            if (!slash) { free(receipt); free(response); return 1; }
            snprintf(slash + 1,
                     sizeof overflow - (size_t)(slash + 1 - overflow),
                     "overflow/%s", ref);
            stream_path = overflow;
        }
        unsigned long long expected_size = 0;
        snprintf(size_key, sizeof size_key, "\"%s_bytes\"", tags[i]);
        if (v_json_size(response, size_key, &expected_size) != 0) {
            free(receipt); free(response); return 1;
        }
        int hash_rc = stream_fds[i] >= 0
            ? v_hash_regular_fd(stream_fds[i], expected_size, got)
            : v_hash_regular_file(stream_path, expected_size, got);
        if (hash_rc != 0) {
            free(receipt); free(response); return 1;
        }
        if (strcmp(got, stream_hashes[i]) != 0) {
            free(receipt); free(response); return 1;
        }
    }
    size_t base_len = (size_t)(marker - response);
    char* base = (char*)malloc(base_len + 3);
    if (!base) { free(receipt); free(response); return -1; }
    memcpy(base, response, base_len);
    base[base_len++] = '}'; base[base_len++] = '\n'; base[base_len] = '\0';
    jash_blake3(base, base_len, digest);
    jash_blake3_hex(digest, got);
    free(base); free(receipt); free(response);
    return strcmp(got, expected_result) == 0 ? 0 : 1;
}

int jef_verify_runtime_receipt(const char* receipt_path,
                               const char* response_path,
                               const char* request_path,
                               const char* expected_pubkey_hex,
                               const char* expected_receipt_ref) {
    return jef_verify_runtime_receipt_with_streams(
        receipt_path, response_path, request_path, expected_pubkey_hex,
        expected_receipt_ref, -1, -1);
}

static int v_json_unique(const char* json, const char* key) {
    return v_json_member(json, key) != NULL;
}

static int v_json_all_unique(const char* json, const char* const* keys,
                             size_t count) {
    for (size_t i = 0; i < count; i++)
        if (!v_json_unique(json, keys[i])) return 0;
    return 1;
}

static int v_json_bool(const char* json, const char* key, int* out) {
    const char* p = v_json_member(json, key);
    if (!p) return -1;
    if (strncmp(p, "true", 4) == 0) {
        *out = 1;
        return 0;
    }
    if (strncmp(p, "false", 5) == 0) {
        *out = 0;
        return 0;
    }
    return -1;
}

static void v_maintenance_request_hash(const char* request, size_t size, char out[65]) {
    size_t leading = strspn(request, " \t\r\n"); request += leading; size -= leading;
    if (size && request[size - 1] == '\n') size--;
    uint8_t digest[32]; jash_blake3(request, size, digest); jash_blake3_hex(digest, out);
}

int jef_verify_artifact_receipt(const char* receipt_path, const char* request_path, const char* artifact_path, const char* key) {
    uint8_t public_key[32];
    if (!key || v_hexdec(key, strnlen(key, 65), public_key, sizeof(public_key))) return -2;
    size_t rn = 0, qn = 0; char* receipt = v_read_alloc(receipt_path, 8192, &rn); char* request = v_read_alloc(request_path, 32768, &qn);
    int result = 1; v_dsse envelope;
    if (!receipt || !request) { result = -1; goto done; }
    if (!v_json_object_exact(receipt, rn) || !v_json_object_exact(request, qn) || verify_dsse(receipt, rn, &envelope) ||
        strcasecmp(envelope.keyid, key) || strcmp(envelope.type, "application/vnd.jash.agent-artifact-receipt.v1+json")) goto done;
    const char* payload = (const char*)envelope.payload;
    if (!v_json_object_exact(payload, envelope.payload_len)) goto done;
    char schema[96], operation[32], request_op[32], id[256], request_id[256], request_hash[65], content_hash[65], ref[160], retention[16], actual[65], name[192];
    unsigned long long size;
    if (v_json_string(payload, "\"schema\"", schema, sizeof(schema)) || strcmp(schema, "jash.agent-artifact-receipt.v1") ||
        v_json_string(payload, "\"operation\"", operation, sizeof(operation)) || strcmp(operation, "data.promote") ||
        v_json_string(request, "\"op\"", request_op, sizeof(request_op)) || strcmp(operation, request_op) ||
        v_json_string(payload, "\"request_id\"", id, sizeof(id)) || v_json_string(request, "\"id\"", request_id, sizeof(request_id)) || strcmp(id, request_id) ||
        v_json_string(payload, "\"request_blake3\"", request_hash, sizeof(request_hash)) ||
        v_json_string(payload, "\"content_blake3\"", content_hash, sizeof(content_hash)) ||
        v_json_string(payload, "\"artifact_ref\"", ref, sizeof(ref)) ||
        v_json_string(payload, "\"retention\"", retention, sizeof(retention)) || strcmp(retention, "keep") ||
        v_json_size(payload, "\"bytes\"", &size) || size > 16u * 1024u * 1024u) goto done;
    v_maintenance_request_hash(request, qn, actual); if (strcmp(actual, request_hash)) goto done;
    if (v_hash_regular_file(artifact_path, size, actual) || strcmp(actual, content_hash)) goto done;
    snprintf(name, sizeof(name), "a:blake3-%s-%llu.bin", content_hash, size); if (strcmp(name, ref)) goto done;
    const char* base = strrchr(artifact_path, '/'); base = base ? base + 1 : artifact_path;
    if (strcmp(base, ref + 2)) goto done;
    snprintf(name, sizeof(name), "%s.%s.artifact.dsse", request_hash, content_hash);
    base = strrchr(receipt_path, '/'); base = base ? base + 1 : receipt_path;
    if (strcmp(base, name)) goto done;
    result = 0;
done: free(receipt); free(request); return result;
}

static int v_maintenance_protocol(const char* payload) {
    const char* field = "\"effect_protocol\"", *value = NULL, *cursor = payload;
    if (v_json_members(&cursor, &field, &value, 1, 0) || *cursor) return -1;
    if (!value) return 0;
    char version[32];
    return !v_json_text(&value, version, sizeof(version)) && !strcmp(version, "admission-v1") ? 1 : -1;
}

static int v_maintenance_sibling(const char* path, const char* name, char out[4096]) {
    const char* slash = strrchr(path, '/');
    int n = slash ? snprintf(out, 4096, "%.*s/%s", (int)(slash - path), path, name) : snprintf(out, 4096, "%s", name);
    return n < 0 || n >= 4096 ? -1 : 0;
}

int jef_verify_maintenance_intent_protocol(const char* receipt_path, const char* request_path, const char* key, int* protocol) {
    uint8_t public_key[ED25519_PUBKEY];
    if (!key || v_hexdec(key, strnlen(key, 65), public_key, sizeof(public_key))) return -2;
    size_t rn = 0, qn = 0;
    char* receipt = v_read_alloc(receipt_path, 8192, &rn);
    char* request = v_read_alloc(request_path, 32768, &qn);
    int result = 1; v_dsse preparation;
    if (!receipt || !request) { result = -1; goto done; }
    if (!v_json_object_exact(receipt, rn) || !v_json_object_exact(request, qn) ||
        verify_dsse(receipt, rn, &preparation) || strcasecmp(preparation.keyid, key) ||
        strcmp(preparation.type, "application/vnd.jash.agent-maintenance-receipt.v1+json")) goto done;
    const char* payload = (const char*)preparation.payload;
    const char* fields[] = {"\"schema\"", "\"phase\"", "\"request_id\"", "\"operation\"", "\"request_blake3\""};
    if (!v_json_object_exact(payload, preparation.payload_len) || !v_json_all_unique(payload, fields, 5) ||
        !v_json_unique(request, "\"id\"") || !v_json_unique(request, "\"op\"")) goto done;
    char schema[96], phase[32], id[256], qid[256], op[32], qop[32], hash[65], got[65], name[128];
    if (v_json_string(payload, "\"schema\"", schema, sizeof(schema)) || strcmp(schema, "jash.agent-maintenance-receipt.v1") ||
        v_json_string(payload, "\"phase\"", phase, sizeof(phase)) || strcmp(phase, "intent") ||
        v_json_string(payload, "\"request_id\"", id, sizeof(id)) || v_json_string(request, "\"id\"", qid, sizeof(qid)) || strcmp(id, qid) ||
        v_json_string(payload, "\"operation\"", op, sizeof(op)) || v_json_string(request, "\"op\"", qop, sizeof(qop)) || strcmp(op, qop) ||
        (strcmp(op, "store.gc") && strcmp(op, "store.quota") && strcmp(op, "store.release") && strcmp(op, "store.journal.quota") && strcmp(op, "store.journal.gc")) ||
        v_json_string(payload, "\"request_blake3\"", hash, sizeof(hash))) goto done;
    v_maintenance_request_hash(request, qn, got); if (strcmp(got, hash)) goto done;
    snprintf(name, sizeof(name), "%s.maintenance-intent.dsse", got);
    const char* basename = strrchr(receipt_path, '/'); basename = basename ? basename + 1 : receipt_path;
    if (strcmp(basename, name)) goto done;
    int version = v_maintenance_protocol(payload); if (version < 0) goto done;
    if (protocol) *protocol = version;
    result = 0;
done:
    free(receipt); free(request); return result;
}

int jef_verify_maintenance_intent(const char* receipt_path, const char* request_path, const char* key) {
    return jef_verify_maintenance_intent_protocol(receipt_path, request_path, key, NULL);
}

int jef_verify_maintenance_decision(const char* receipt_path, const char* request_path, const char* key, int* admitted) {
    uint8_t public_key[32], digest[32];
    if (!key || v_hexdec(key, strnlen(key, 65), public_key, sizeof(public_key))) return -2;
    size_t rn = 0, qn = 0, in = 0;
    char* receipt = v_read_alloc(receipt_path, 8192, &rn);
    char* request = v_read_alloc(request_path, 32768, &qn);
    char* intent = NULL; int result = 1; v_dsse decision;
    if (!receipt || !request) { result = -1; goto done; }
    if (!v_json_object_exact(receipt, rn) || !v_json_object_exact(request, qn) || verify_dsse(receipt, rn, &decision) ||
        strcasecmp(decision.keyid, key) || strcmp(decision.type, "application/vnd.jash.agent-maintenance-receipt.v1+json")) goto done;
    const char* payload = (const char*)decision.payload;
    const char* fields[] = {"\"schema\"", "\"phase\"", "\"request_id\"", "\"operation\"", "\"request_blake3\"", "\"intent_ref\"", "\"intent_blake3\"", "\"decision\""};
    if (!v_json_object_exact(payload, decision.payload_len) || !v_json_all_unique(payload, fields, 8)) goto done;
    char schema[96], phase[32], id[256], qid[256], op[32], qop[32], hash[65], got[65], name[128];
    char intent_ref[128], intent_hash[65], choice[32], path[4096]; int protocol;
    if (v_json_string(payload, "\"schema\"", schema, sizeof(schema)) || strcmp(schema, "jash.agent-maintenance-receipt.v1") ||
        v_json_string(payload, "\"phase\"", phase, sizeof(phase)) || strcmp(phase, "decision") ||
        v_json_string(payload, "\"request_id\"", id, sizeof(id)) || v_json_string(request, "\"id\"", qid, sizeof(qid)) || strcmp(id, qid) ||
        v_json_string(payload, "\"operation\"", op, sizeof(op)) || v_json_string(request, "\"op\"", qop, sizeof(qop)) || strcmp(op, qop) ||
        v_json_string(payload, "\"request_blake3\"", hash, sizeof(hash)) ||
        v_json_string(payload, "\"intent_ref\"", intent_ref, sizeof(intent_ref)) ||
        v_json_string(payload, "\"intent_blake3\"", intent_hash, sizeof(intent_hash)) ||
        v_json_string(payload, "\"decision\"", choice, sizeof(choice)) || (strcmp(choice, "admitted") && strcmp(choice, "closed"))) goto done;
    v_maintenance_request_hash(request, qn, got); if (strcmp(got, hash)) goto done;
    snprintf(name, sizeof(name), "%s.maintenance-decision.dsse", hash);
    const char* basename = strrchr(receipt_path, '/'); basename = basename ? basename + 1 : receipt_path;
    if (strcmp(basename, name)) goto done;
    snprintf(name, sizeof(name), "%s.maintenance-intent.dsse", hash);
    if (strcmp(name, intent_ref) || v_maintenance_sibling(receipt_path, name, path) ||
        jef_verify_maintenance_intent_protocol(path, request_path, key, &protocol) || protocol != 1) goto done;
    intent = v_read_alloc(path, 8192, &in); if (!intent) { result = -1; goto done; }
    jash_blake3(intent, in, digest); jash_blake3_hex(digest, got); if (strcmp(got, intent_hash)) goto done;
    if (admitted) *admitted = !strcmp(choice, "admitted");
    result = 0;
done:
    free(receipt); free(request); free(intent); return result;
}

int jef_verify_maintenance_receipt(const char* receipt_path, const char* response_path,
                                  const char* request_path, const char* key) {
    uint8_t public_key[ED25519_PUBKEY];
    if (!key || v_hexdec(key, strnlen(key, 65), public_key, sizeof(public_key))) return -2;
    size_t rn = 0, sn = 0, qn = 0, in = 0;
    char* receipt = v_read_alloc(receipt_path, 8192, &rn);
    char* response = v_read_alloc(response_path, JEF_MAINTENANCE_RESPONSE_MAX, &sn);
    char* request = v_read_alloc(request_path, 32768, &qn);
    char* intent = NULL; int result = 1;
    if (!receipt || !response || !request) { result = -1; goto done; }
    v_dsse terminal, preparation;
    const char* type = "application/vnd.jash.agent-maintenance-receipt.v1+json";
    if (!v_json_object_exact(receipt, rn) || !v_json_object_exact(response, sn) || !v_json_object_exact(request, qn) ||
        verify_dsse(receipt, rn, &terminal) || strcasecmp(terminal.keyid, key) || strcmp(terminal.type, type)) goto done;
    const char* payload = (const char*)terminal.payload;
    if (!v_json_object_exact(payload, terminal.payload_len)) goto done;
    const char* payload_keys[] = {"\"schema\"", "\"phase\"", "\"request_id\"", "\"operation\"", "\"request_blake3\"", "\"response_blake3\"", "\"intent_ref\"", "\"intent_blake3\""};
    const char* response_keys[] = {"\"id\"", "\"op\"", "\"transaction\"", "\"receipt_phase\"", "\"receipt_ref\"", "\"receipt_blake3\"", "\"receipt_pubkey\"", "\"schema\"", "\"ok\"", "\"applied\""};
    if (!v_json_all_unique(payload, payload_keys, 8) || !v_json_all_unique(response, response_keys, 10) ||
        !v_json_unique(request, "\"id\"") || !v_json_unique(request, "\"op\"")) goto done;
    char schema[96], phase[32], id[256], rid[256], qid[256], op[32], rop[32], qop[32], transaction[32];
    char request_hash[65], response_hash[65], receipt_hash[65], receipt_ref[256], response_key[65];
    char intent_ref[256], intent_hash[65], got[65], expected[256]; uint8_t digest[32];
    int verdict, applied;
    if (v_json_bool(response, "\"ok\"", &verdict) || v_json_bool(response, "\"applied\"", &applied) ||
        v_json_string(response, "\"schema\"", schema, sizeof(schema)) || strcmp(schema, "jash.agent-store.v1") ||
        v_json_string(payload, "\"schema\"", schema, sizeof(schema)) || strcmp(schema, "jash.agent-maintenance-receipt.v1") ||
        v_json_string(payload, "\"phase\"", phase, sizeof(phase)) || strcmp(phase, "terminal") ||
        v_json_string(payload, "\"request_id\"", id, sizeof(id)) || v_json_string(request, "\"id\"", qid, sizeof(qid)) ||
        v_json_string(response, "\"id\"", rid, sizeof(rid)) || strcmp(id, qid) || strcmp(id, rid) ||
        v_json_string(payload, "\"operation\"", op, sizeof(op)) || v_json_string(request, "\"op\"", qop, sizeof(qop)) ||
        v_json_string(response, "\"op\"", rop, sizeof(rop)) || strcmp(op, rop) || strcmp(op, qop) ||
        (strcmp(op, "store.gc") && strcmp(op, "store.quota") && strcmp(op, "store.release") && strcmp(op, "store.journal.quota") && strcmp(op, "store.journal.gc")) ||
        v_json_string(response, "\"transaction\"", transaction, sizeof(transaction)) || strcmp(transaction, "committed") ||
        v_json_string(response, "\"receipt_phase\"", phase, sizeof(phase)) || strcmp(phase, "maintenance") ||
        v_json_string(payload, "\"request_blake3\"", request_hash, sizeof(request_hash)) ||
        v_json_string(payload, "\"response_blake3\"", response_hash, sizeof(response_hash)) ||
        v_json_string(payload, "\"intent_ref\"", intent_ref, sizeof(intent_ref)) ||
        v_json_string(payload, "\"intent_blake3\"", intent_hash, sizeof(intent_hash)) ||
        v_json_string(response, "\"receipt_ref\"", receipt_ref, sizeof(receipt_ref)) ||
        v_json_string(response, "\"receipt_blake3\"", receipt_hash, sizeof(receipt_hash)) ||
        v_json_string(response, "\"receipt_pubkey\"", response_key, sizeof(response_key)) || strcasecmp(response_key, key)) goto done;
    const char* basename = strrchr(receipt_path, '/'); basename = basename ? basename + 1 : receipt_path;
    if (strcmp(basename, receipt_ref)) goto done;
    jash_blake3(receipt, rn, digest); jash_blake3_hex(digest, got); if (strcmp(got, receipt_hash)) goto done;
    v_maintenance_request_hash(request, qn, got); if (strcmp(got, request_hash)) goto done;
    const char* marker = strstr(response, ",\"receipt_phase\":\"maintenance\""); char tail[1024];
    int length = snprintf(tail, sizeof(tail), ",\"receipt_phase\":\"maintenance\",\"receipt_ref\":\"%s\",\"receipt_blake3\":\"%s\",\"receipt_pubkey\":\"%s\"}\n", receipt_ref, receipt_hash, response_key);
    if (!marker || length < 0 || (size_t)length >= sizeof(tail) || strcmp(marker, tail)) goto done;
    struct blake3 hash; blake3_init(&hash); blake3_update(&hash, response, (uint64_t)(marker - response)); blake3_update(&hash, "}\n", 2);
    blake3_final(&hash, digest, sizeof(digest)); jash_blake3_hex(digest, got); if (strcmp(got, response_hash)) goto done;
    snprintf(expected, sizeof(expected), "%s.%s.maintenance-terminal.dsse", request_hash, response_hash);
    if (strcmp(expected, receipt_ref)) goto done;
    snprintf(expected, sizeof(expected), "%s.maintenance-intent.dsse", request_hash);
    if (strcmp(expected, intent_ref)) goto done;
    char path[4096]; const char* slash = strrchr(receipt_path, '/');
    int n = slash ? snprintf(path, sizeof(path), "%.*s/%s", (int)(slash - receipt_path), receipt_path, intent_ref) : snprintf(path, sizeof(path), "%s", intent_ref);
    if (n < 0 || (size_t)n >= sizeof(path)) goto done;
    intent = v_read_alloc(path, 8192, &in);
    if (!intent || verify_dsse(intent, in, &preparation) || strcasecmp(preparation.keyid, key) || strcmp(preparation.type, type)) goto done;
    jash_blake3(intent, in, digest); jash_blake3_hex(digest, got); if (strcmp(got, intent_hash)) goto done;
    /* INV-877: terminal and preparation are different stack-owned DSSE
     * envelopes. Verify the second one through its own explicit origin. */
    if (!v_json_object_exact((const char*)preparation.payload, preparation.payload_len) ||
        !v_json_all_unique((const char*)preparation.payload, payload_keys, 5) ||
        v_json_string((const char*)preparation.payload, "\"schema\"", schema, sizeof(schema)) || strcmp(schema, "jash.agent-maintenance-receipt.v1") ||
        v_json_string((const char*)preparation.payload, "\"phase\"", phase, sizeof(phase)) || strcmp(phase, "intent") ||
        v_json_string((const char*)preparation.payload, "\"request_id\"", rid, sizeof(rid)) || strcmp(rid, id) ||
        v_json_string((const char*)preparation.payload, "\"operation\"", rop, sizeof(rop)) || strcmp(rop, op) ||
        v_json_string((const char*)preparation.payload, "\"request_blake3\"", got, sizeof(got)) || strcmp(got, request_hash)) goto done;
    int protocol = v_maintenance_protocol((const char*)preparation.payload);
    if (protocol < 0) goto done;
    if (protocol == 1) {
        char decision_ref[128], decision_hash[65], admission[32]; int admitted;
        const char* terminal_payload = (const char*)terminal.payload;
        if (v_json_string(terminal_payload, "\"decision_ref\"", decision_ref, sizeof(decision_ref)) ||
            v_json_string(terminal_payload, "\"decision_blake3\"", decision_hash, sizeof(decision_hash))) goto done;
        snprintf(expected, sizeof(expected), "%s.maintenance-decision.dsse", request_hash);
        if (strcmp(decision_ref, expected) || v_maintenance_sibling(receipt_path, decision_ref, path) ||
            jef_verify_maintenance_decision(path, request_path, key, &admitted)) goto done;
        size_t dn; char* decision = v_read_alloc(path, 8192, &dn); if (!decision) goto done;
        jash_blake3(decision, dn, digest); jash_blake3_hex(digest, got); free(decision);
        if (strcmp(got, decision_hash) || v_json_string(response, "\"effect_admission\"", admission, sizeof(admission)) ||
            strcmp(admission, admitted ? "admitted" : "closed")) goto done;
        if (!admitted) {
            char error[64];
            if (verdict || applied || v_json_string(response, "\"error\"", error, sizeof(error)) || strcmp(error, "effects_not_admitted")) goto done;
        }
    }
    result = 0;
done:
    free(receipt); free(response); free(request); free(intent); return result;
}

static int v_job_payload(const v_dsse* envelope, char* payload, size_t cap) {
    static const char media[] =
        "application/vnd.jash.agent-job-receipt.v1+json";
    if (strcmp(envelope->type, media) != 0 || envelope->payload_len >= cap ||
        !v_json_object_exact((const char*)envelope->payload, envelope->payload_len))
        return 1;
    memcpy(payload, envelope->payload, envelope->payload_len + 1);
    char schema[96];
    if (!v_json_unique(payload, "\"schema\"") ||
        v_json_string(payload, "\"schema\"", schema, sizeof schema) != 0 ||
        strcmp(schema, "jash.agent-job-receipt.v1") != 0)
        return 1;
    return 0;
}

static int v_job_stream(const char* receipt_path, const char* response,
                        const char* payload, const char* tag, int stream_fd) {
    char ref_key[40], hash_key[40], bytes_key[40];
    char response_ref[256], payload_ref[256];
    char response_hash[65], payload_hash[65], deterministic[384], got[65];
    snprintf(ref_key, sizeof ref_key, "\"%s_ref\"", tag);
    snprintf(hash_key, sizeof hash_key, "\"%s_blake3\"", tag);
    snprintf(bytes_key, sizeof bytes_key, "\"%s_bytes\"", tag);
    unsigned long long response_bytes = 0, payload_bytes = 0;
    if (!v_json_unique(response, ref_key) || !v_json_unique(payload, ref_key) ||
        !v_json_unique(response, hash_key) || !v_json_unique(payload, hash_key) ||
        !v_json_unique(response, bytes_key) || !v_json_unique(payload, bytes_key) ||
        v_json_string(response, ref_key, response_ref, sizeof response_ref) != 0 ||
        v_json_string(payload, ref_key, payload_ref, sizeof payload_ref) != 0 ||
        v_json_string(response, hash_key, response_hash,
                      sizeof response_hash) != 0 ||
        v_json_string(payload, hash_key, payload_hash,
                      sizeof payload_hash) != 0 ||
        v_json_size(response, bytes_key, &response_bytes) != 0 ||
        v_json_size(payload, bytes_key, &payload_bytes) != 0 ||
        strcmp(response_ref, payload_ref) != 0 ||
        strcmp(response_hash, payload_hash) != 0 ||
        response_bytes != payload_bytes || strlen(payload_hash) != 64)
        return 1;
    int rn = snprintf(deterministic, sizeof deterministic,
                      "blake3-%s-%llu.bin", payload_hash, payload_bytes);
    if (rn <= 0 || (size_t)rn >= sizeof deterministic ||
        strcmp(payload_ref, deterministic) != 0)
        return 1;

    if (stream_fd >= 0)
        return v_hash_regular_fd(stream_fd, payload_bytes, got) == 0 &&
               strcmp(got, payload_hash) == 0 ? 0 : 1;
    char path[1024];
    int pn = snprintf(path, sizeof path, "%s", receipt_path);
    if (pn <= 0 || (size_t)pn >= sizeof path) return 1;
    char* slash = strrchr(path, '/');
    if (!slash) return 1;
    *slash = '\0';
    slash = strrchr(path, '/');
    if (!slash) return 1;
    pn = snprintf(slash + 1, sizeof path - (size_t)(slash + 1 - path),
                  "overflow/%s", payload_ref);
    if (pn <= 0 || (size_t)pn >= sizeof path - (size_t)(slash + 1 - path))
        return 1;
    return v_hash_regular_file(path, payload_bytes, got) == 0 &&
           strcmp(got, payload_hash) == 0 ? 0 : 1;
}

static int v_job_start_link(const char* terminal_path, const char* start_ref,
                            const char* expected_hash, const char* pubkey,
                            const char* request_hash, const char* job_id) {
    char path[1024];
    int pn = snprintf(path, sizeof path, "%s", terminal_path);
    if (pn <= 0 || (size_t)pn >= sizeof path) return 1;
    char* slash = strrchr(path, '/');
    if (!slash || strchr(start_ref, '/') || strstr(start_ref, "..")) return 1;
    pn = snprintf(slash + 1, sizeof path - (size_t)(slash + 1 - path),
                  "%s", start_ref);
    if (pn <= 0 || (size_t)pn >= sizeof path - (size_t)(slash + 1 - path))
        return 1;
    size_t length = 0;
    char* receipt = v_read_alloc(path, 16384, &length);
    if (!receipt) return 1;
    uint8_t digest[32]; char got[65], payload[4096];
    jash_blake3(receipt, length, digest);
    jash_blake3_hex(digest, got);
    v_dsse envelope;
    int ok = v_json_object_exact(receipt, length) &&
             strcmp(got, expected_hash) == 0 &&
             pubkey && verify_dsse(receipt, length, &envelope) == 0 &&
             strcasecmp(envelope.keyid, pubkey) == 0 &&
             v_job_payload(&envelope, payload, sizeof payload) == 0;
    free(receipt);
    if (!ok) return 1;
    char phase[16], linked_request[65], linked_job[64];
    if (!v_json_unique(payload, "\"phase\"") ||
        !v_json_unique(payload, "\"request_blake3\"") ||
        !v_json_unique(payload, "\"job_id\"") ||
        v_json_string(payload, "\"phase\"", phase, sizeof phase) != 0 ||
        strcmp(phase, "start") != 0 ||
        v_json_string(payload, "\"request_blake3\"", linked_request,
                      sizeof linked_request) != 0 ||
        v_json_string(payload, "\"job_id\"", linked_job,
                      sizeof linked_job) != 0)
        return 1;
    return strcmp(linked_request, request_hash) == 0 &&
           strcmp(linked_job, job_id) == 0 ? 0 : 1;
}

int jef_verify_job_receipt_with_streams(
    const char* receipt_path, const char* response_path,
    const char* start_request_path, const char* expected_pubkey_hex,
    const char* expected_phase, const char* expected_receipt_ref,
    int stdout_fd, int stderr_fd) {
    if (!expected_phase || (strcmp(expected_phase, "start") != 0 &&
                            strcmp(expected_phase, "action") != 0 &&
                            strcmp(expected_phase, "terminal") != 0))
        return 1;
    size_t receipt_len = 0, response_len = 0, request_len = 0;
    char* receipt = v_read_alloc(receipt_path, 16384, &receipt_len);
    char* response = v_read_alloc(response_path, 600 * 1024, &response_len);
    char* request = v_read_alloc(start_request_path, 32 * 1024, &request_len);
    if (!receipt || !response || !request) {
        free(receipt); free(response); free(request); return -1;
    }
    v_dsse envelope;
    if (!v_json_object_exact(receipt, receipt_len) ||
        !v_json_object_exact(response, response_len) ||
        !v_json_object_exact(request, request_len) ||
        !expected_pubkey_hex || strnlen(expected_pubkey_hex, 65) != 64 ||
        verify_dsse(receipt, receipt_len, &envelope) != 0 ||
        strcasecmp(envelope.keyid, expected_pubkey_hex) != 0)
        goto invalid;
    char payload[4096];
    if (v_job_payload(&envelope, payload, sizeof payload) != 0) goto invalid;
    char phase[16], response_phase[16], request_hash[65];
    char job_id[64], response_job[64];
    char receipt_ref[256], response_hash[65], response_key[65];
    if (!v_json_unique(payload, "\"phase\"") ||
        !v_json_unique(payload, "\"request_blake3\"") ||
        !v_json_unique(payload, "\"job_id\"") ||
        !v_json_unique(response, "\"job_id\"") ||
        !v_json_unique(response, "\"receipt_ref\"") ||
        !v_json_unique(response, "\"receipt_blake3\"") ||
        !v_json_unique(response, "\"receipt_pubkey\"") ||
        !v_json_unique(response, "\"receipt_phase\"") ||
        v_json_string(payload, "\"phase\"", phase, sizeof phase) != 0 ||
        strcmp(phase, expected_phase) != 0 ||
        v_json_string(response, "\"receipt_phase\"", response_phase,
                      sizeof response_phase) != 0 ||
        strcmp(response_phase, expected_phase) != 0 ||
        v_json_string(payload, "\"request_blake3\"", request_hash,
                      sizeof request_hash) != 0 ||
        v_json_string(payload, "\"job_id\"", job_id, sizeof job_id) != 0 ||
        v_json_string(response, "\"job_id\"", response_job,
                      sizeof response_job) != 0 ||
        strcmp(job_id, response_job) != 0 ||
        v_json_string(response, "\"receipt_ref\"", receipt_ref,
                      sizeof receipt_ref) != 0 ||
        v_json_string(response, "\"receipt_blake3\"", response_hash,
                      sizeof response_hash) != 0 ||
        v_json_string(response, "\"receipt_pubkey\"", response_key,
                      sizeof response_key) != 0 ||
        strcmp(response_key, expected_pubkey_hex) != 0)
        goto invalid;
    const char* receipt_name = expected_receipt_ref;
    if (!receipt_name) {
        receipt_name = strrchr(receipt_path, '/');
        receipt_name = receipt_name ? receipt_name + 1 : receipt_path;
    }
    if (strcmp(receipt_ref, receipt_name) != 0) goto invalid;
    uint8_t digest[32]; char got[65], deterministic[224];
    jash_blake3(receipt, receipt_len, digest);
    jash_blake3_hex(digest, got);
    if (strcmp(got, response_hash) != 0) goto invalid;
    if (request_len && request[request_len - 1] == '\n') request_len--;
    jash_blake3(request, request_len, digest);
    jash_blake3_hex(digest, got);
    if (strcmp(got, request_hash) != 0) goto invalid;
    if (strcmp(expected_phase, "start") == 0) {
        static const char* const payload_keys[] = {
            "\"response_blake3\"", "\"accepted\"", "\"terminal\"",
        };
        static const char* const response_keys[] = {
            "\"accepted\"", "\"terminal\"",
        };
        char expected_response[65];
        int accepted_payload = 0, terminal_payload = 1;
        int accepted_response = 0, terminal_response = 1;
        if (!v_json_all_unique(payload, payload_keys,
                               sizeof payload_keys / sizeof payload_keys[0]) ||
            !v_json_all_unique(response, response_keys,
                               sizeof response_keys / sizeof response_keys[0]) ||
            v_json_string(payload, "\"response_blake3\"", expected_response,
                          sizeof expected_response) != 0 ||
            v_json_bool(payload, "\"accepted\"", &accepted_payload) != 0 ||
            v_json_bool(payload, "\"terminal\"", &terminal_payload) != 0 ||
            v_json_bool(response, "\"accepted\"", &accepted_response) != 0 ||
            v_json_bool(response, "\"terminal\"", &terminal_response) != 0 ||
            !accepted_payload || terminal_payload || !accepted_response ||
            terminal_response)
            goto invalid;
        const char* marker = strstr(response, ",\"receipt_phase\":\"start\"");
        char expected_tail[1024];
        int tn = snprintf(expected_tail, sizeof expected_tail,
            ",\"receipt_phase\":\"start\",\"receipt_ref\":\"%s\","
            "\"receipt_blake3\":\"%s\",\"receipt_pubkey\":\"%s\"}\n",
            receipt_ref, response_hash, response_key);
        if (!marker || tn <= 0 || (size_t)tn >= sizeof expected_tail ||
            strcmp(marker, expected_tail) != 0)
            goto invalid;
        size_t base_len = (size_t)(marker - response);
        char* base = malloc(base_len + 3u);
        if (!base) { free(receipt); free(response); free(request); return -1; }
        memcpy(base, response, base_len);
        base[base_len++] = '}'; base[base_len++] = '\n'; base[base_len] = '\0';
        jash_blake3(base, base_len, digest);
        jash_blake3_hex(digest, got);
        free(base);
        int dn = snprintf(deterministic, sizeof deterministic,
                          "%s.job-start.dsse", request_hash);
        if (strcmp(got, expected_response) != 0 || dn <= 0 ||
            (size_t)dn >= sizeof deterministic ||
            strcmp(receipt_ref, deterministic) != 0)
            goto invalid;
    } else if (strcmp(expected_phase, "action") == 0) {
        static const char* const action_keys[] = {
            "\"response_blake3\"", "\"op\"", "\"start_request_blake3\"",
            "\"start_receipt_ref\"", "\"start_receipt_blake3\"",
        };
        char expected_response[65], op[16], response_op[16], request_op[16];
        char start_request_hash[65];
        char payload_start_ref[256], payload_start_hash[65];
        char request_job[64], deterministic_start[96];
        if (!v_json_all_unique(payload, action_keys,
                               sizeof action_keys / sizeof action_keys[0]) ||
            !v_json_unique(response, "\"op\"") ||
            !v_json_unique(request, "\"op\"") ||
            !v_json_unique(request, "\"job_id\"") ||
            v_json_string(payload, "\"response_blake3\"", expected_response,
                          sizeof expected_response) != 0 ||
            v_json_string(payload, "\"op\"", op, sizeof op) != 0 ||
            (strcmp(op, "job.write") != 0 &&
             strcmp(op, "job.resize") != 0) ||
            v_json_string(response, "\"op\"", response_op,
                          sizeof response_op) != 0 ||
            v_json_string(request, "\"op\"", request_op,
                          sizeof request_op) != 0 ||
            strcmp(op, response_op) != 0 || strcmp(op, request_op) != 0 ||
            v_json_string(request, "\"job_id\"", request_job,
                          sizeof request_job) != 0 ||
            strcmp(request_job, job_id) != 0 ||
            v_json_string(payload, "\"start_request_blake3\"",
                          start_request_hash, sizeof start_request_hash) != 0 ||
            v_json_string(payload, "\"start_receipt_ref\"",
                          payload_start_ref, sizeof payload_start_ref) != 0 ||
            v_json_string(payload, "\"start_receipt_blake3\"",
                          payload_start_hash, sizeof payload_start_hash) != 0 ||
            snprintf(deterministic_start, sizeof deterministic_start,
                     "%s.job-start.dsse", start_request_hash) <= 0 ||
            strcmp(payload_start_ref, deterministic_start) != 0 ||
            v_job_start_link(receipt_path, payload_start_ref,
                             payload_start_hash,
                             expected_pubkey_hex, start_request_hash,
                             job_id) != 0)
            goto invalid;

        unsigned long long first_a, first_b, second_a, second_b;
        if (strcmp(op, "job.write") == 0) {
            if (!v_json_unique(payload, "\"accepted_bytes\"") ||
                !v_json_unique(response, "\"accepted_bytes\"") ||
                strstr(payload, "\"rows\"") || strstr(payload, "\"cols\"") ||
                v_json_size(payload, "\"accepted_bytes\"", &first_a) != 0 ||
                v_json_size(response, "\"accepted_bytes\"", &first_b) != 0 ||
                first_a != first_b)
                goto invalid;
        } else if (!v_json_unique(payload, "\"rows\"") ||
                   !v_json_unique(payload, "\"cols\"") ||
                   !v_json_unique(response, "\"rows\"") ||
                   !v_json_unique(response, "\"cols\"") ||
                   strstr(payload, "\"accepted_bytes\"") ||
                   v_json_size(payload, "\"rows\"", &first_a) != 0 ||
                   v_json_size(response, "\"rows\"", &first_b) != 0 ||
                   v_json_size(payload, "\"cols\"", &second_a) != 0 ||
                   v_json_size(response, "\"cols\"", &second_b) != 0 ||
                   first_a != first_b || second_a != second_b) {
            goto invalid;
        }

        const char* marker = strstr(response, ",\"receipt_phase\":\"action\"");
        char expected_tail[1024];
        int tn = snprintf(expected_tail, sizeof expected_tail,
            ",\"receipt_phase\":\"action\",\"receipt_ref\":\"%s\","
            "\"receipt_blake3\":\"%s\",\"receipt_pubkey\":\"%s\"}\n",
            receipt_ref, response_hash, response_key);
        if (!marker || tn <= 0 || (size_t)tn >= sizeof expected_tail ||
            strcmp(marker, expected_tail) != 0)
            goto invalid;
        size_t base_len = (size_t)(marker - response);
        char* base = malloc(base_len + 3u);
        if (!base) { free(receipt); free(response); free(request); return -1; }
        memcpy(base, response, base_len);
        base[base_len++] = '}'; base[base_len++] = '\n'; base[base_len] = '\0';
        jash_blake3(base, base_len, digest);
        jash_blake3_hex(digest, got);
        free(base);
        int dn = snprintf(deterministic, sizeof deterministic,
                          "%s.job-action.dsse", request_hash);
        if (strcmp(got, expected_response) != 0 || dn <= 0 ||
            (size_t)dn >= sizeof deterministic ||
            strcmp(receipt_ref, deterministic) != 0)
            goto invalid;
    } else {
        static const char* const terminal_keys[] = {
            "\"state\"", "\"rc\"", "\"signal\"", "\"quiescent\"",
            "\"quiescence_forced\"", "\"cancel_requested\"",
            "\"kill_sent\"", "\"output_limit\"",
            "\"output_limit_bytes\"", "\"start_receipt_ref\"",
            "\"start_receipt_blake3\"", "\"start_request_blake3\"",
        };
        static const char* const framing_keys[] = {
            "\"id\"", "\"protocol_version\"", "\"op\"",
            "\"background\"", "\"pty\"",
        };
        char state[16], start_ref[256], start_hash[65];
        char payload_start_ref[256], payload_start_hash[65];
        char deterministic_start[96], expected_response[65], op[16];
        char start_request_hash[65], response_start_request_hash[65];
        char request_op[16], request_job[64];
        unsigned long long rc_a, rc_b, sig_a, sig_b, limit_bytes_a, limit_bytes_b;
        unsigned long long protocol_version;
        int terminal, quiet_a, quiet_b, forced_a, forced_b, background, pty;
        int cancel_a, cancel_b, kill_a, kill_b, limit_a, limit_b;
        if (!v_json_unique(payload, "\"response_blake3\"") ||
            !v_json_all_unique(payload, terminal_keys,
                               sizeof terminal_keys / sizeof terminal_keys[0]) ||
            !v_json_all_unique(response, terminal_keys,
                               sizeof terminal_keys / sizeof terminal_keys[0]) ||
            !v_json_all_unique(response, framing_keys,
                               sizeof framing_keys / sizeof framing_keys[0]) ||
            !v_json_unique(response, "\"terminal\"") ||
            v_json_string(payload, "\"response_blake3\"", expected_response,
                          sizeof expected_response) != 0 ||
            strlen(expected_response) != 64 ||
            v_json_size(response, "\"protocol_version\"",
                        &protocol_version) != 0 || protocol_version != 2 ||
            v_json_string(response, "\"op\"", op, sizeof op) != 0 ||
            (strcmp(op, "exec") != 0 && strcmp(op, "job.get") != 0 &&
             strcmp(op, "job.wait") != 0 && strcmp(op, "job.cancel") != 0) ||
            v_json_string(request, "\"op\"", request_op,
                          sizeof request_op) != 0 ||
            strcmp(op, request_op) != 0 ||
            v_json_bool(response, "\"background\"", &background) != 0 ||
            v_json_bool(response, "\"pty\"", &pty) != 0 ||
            v_json_string(payload, "\"state\"", state, sizeof state) != 0 ||
            strcmp(state, "done") != 0 ||
            v_json_string(response, "\"state\"", state, sizeof state) != 0 ||
            strcmp(state, "done") != 0 ||
            v_json_bool(response, "\"terminal\"", &terminal) != 0 || !terminal ||
            v_json_size(payload, "\"rc\"", &rc_a) != 0 ||
            v_json_size(response, "\"rc\"", &rc_b) != 0 || rc_a != rc_b ||
            v_json_size(payload, "\"signal\"", &sig_a) != 0 ||
            v_json_size(response, "\"signal\"", &sig_b) != 0 || sig_a != sig_b ||
            v_json_bool(payload, "\"quiescent\"", &quiet_a) != 0 ||
            v_json_bool(response, "\"quiescent\"", &quiet_b) != 0 ||
            !quiet_a || !quiet_b ||
            v_json_bool(payload, "\"quiescence_forced\"", &forced_a) != 0 ||
            v_json_bool(response, "\"quiescence_forced\"", &forced_b) != 0 ||
            forced_a != forced_b ||
            v_json_bool(payload, "\"cancel_requested\"", &cancel_a) != 0 ||
            v_json_bool(response, "\"cancel_requested\"", &cancel_b) != 0 ||
            cancel_a != cancel_b ||
            v_json_bool(payload, "\"kill_sent\"", &kill_a) != 0 ||
            v_json_bool(response, "\"kill_sent\"", &kill_b) != 0 ||
            kill_a != kill_b ||
            v_json_bool(payload, "\"output_limit\"", &limit_a) != 0 ||
            v_json_bool(response, "\"output_limit\"", &limit_b) != 0 ||
            limit_a != limit_b ||
            v_json_size(payload, "\"output_limit_bytes\"", &limit_bytes_a) != 0 ||
            v_json_size(response, "\"output_limit_bytes\"", &limit_bytes_b) != 0 ||
            limit_bytes_a != limit_bytes_b ||
            v_json_string(payload, "\"start_receipt_ref\"", payload_start_ref,
                          sizeof payload_start_ref) != 0 ||
            v_json_string(response, "\"start_receipt_ref\"", start_ref,
                          sizeof start_ref) != 0 ||
            v_json_string(payload, "\"start_receipt_blake3\"", payload_start_hash,
                          sizeof payload_start_hash) != 0 ||
            v_json_string(response, "\"start_receipt_blake3\"", start_hash,
                          sizeof start_hash) != 0 ||
            v_json_string(payload, "\"start_request_blake3\"",
                          start_request_hash, sizeof start_request_hash) != 0 ||
            v_json_string(response, "\"start_request_blake3\"",
                          response_start_request_hash,
                          sizeof response_start_request_hash) != 0 ||
            strcmp(start_request_hash, response_start_request_hash) != 0 ||
            strcmp(start_ref, payload_start_ref) != 0 ||
            strcmp(start_hash, payload_start_hash) != 0 ||
            snprintf(deterministic_start, sizeof deterministic_start,
                     "%s.job-start.dsse", start_request_hash) <= 0 ||
            strcmp(start_ref, deterministic_start) != 0 ||
            v_job_start_link(receipt_path, start_ref, start_hash,
                             expected_pubkey_hex, start_request_hash,
                             job_id) != 0 ||
            v_job_stream(receipt_path, response, payload, "stdout", stdout_fd) != 0 ||
            v_job_stream(receipt_path, response, payload, "stderr", stderr_fd) != 0)
            goto invalid;

        if (strcmp(op, "exec") != 0 &&
            (v_json_string(request, "\"job_id\"", request_job,
                           sizeof request_job) != 0 ||
             strcmp(request_job, job_id) != 0))
            goto invalid;

        int operation_flag;
        if (strcmp(op, "job.wait") == 0) {
            if (!v_json_unique(response, "\"timed_out\"") ||
                v_json_bool(response, "\"timed_out\"", &operation_flag) != 0 ||
                operation_flag)
                goto invalid;
        } else if (strstr(response, "\"timed_out\"") != NULL) {
            goto invalid;
        }
        if (strcmp(op, "job.cancel") == 0) {
            if (!v_json_unique(response, "\"cancel_applied\"") ||
                v_json_bool(response, "\"cancel_applied\"",
                            &operation_flag) != 0)
                goto invalid;
        } else if (strstr(response, "\"cancel_applied\"") != NULL) {
            goto invalid;
        }

        const char* marker = strstr(response,
                                    ",\"receipt_phase\":\"terminal\"");
        char expected_tail[1024];
        int tn = snprintf(expected_tail, sizeof expected_tail,
            ",\"receipt_phase\":\"terminal\",\"receipt_ref\":\"%s\","
            "\"receipt_blake3\":\"%s\",\"receipt_pubkey\":\"%s\"}\n",
            receipt_ref, response_hash, response_key);
        if (!marker || tn <= 0 || (size_t)tn >= sizeof expected_tail ||
            strcmp(marker, expected_tail) != 0)
            goto invalid;
        size_t base_len = (size_t)(marker - response);
        char* base = malloc(base_len + 3u);
        if (!base) { free(receipt); free(response); free(request); return -1; }
        memcpy(base, response, base_len);
        base[base_len++] = '}'; base[base_len++] = '\n'; base[base_len] = '\0';
        jash_blake3(base, base_len, digest);
        jash_blake3_hex(digest, got);
        free(base);
        int dn = snprintf(deterministic, sizeof deterministic,
                          "%s.%s.job-terminal.dsse", request_hash,
                          expected_response);
        if (strcmp(got, expected_response) != 0 || dn <= 0 ||
            (size_t)dn >= sizeof deterministic ||
            strcmp(receipt_ref, deterministic) != 0)
            goto invalid;
    }
    free(receipt); free(response); free(request);
    return 0;

invalid:
    free(receipt); free(response); free(request);
    return 1;
}

int jef_verify_job_receipt(
    const char* receipt_path, const char* response_path,
    const char* start_request_path, const char* expected_pubkey_hex,
    const char* expected_phase, const char* expected_receipt_ref) {
    return jef_verify_job_receipt_with_streams(
        receipt_path, response_path, start_request_path, expected_pubkey_hex,
        expected_phase, expected_receipt_ref, -1, -1);
}

int jef_verify_job_spec(
    const char* expected_pubkey_hex, const char* spec_path,
    const char* start_request_path, const char* start_response_path,
    const char* start_receipt_path, const char* terminal_request_path,
    const char* terminal_response_path, const char* terminal_receipt_path) {
    uint8_t digest[32];
    char pubkey[65];
    if (!expected_pubkey_hex ||
        v_hexdec(expected_pubkey_hex, strnlen(expected_pubkey_hex, 65),
                 digest, 32) != 0) return -2;
    jash_sha256_hex(digest, pubkey);
    if (!spec_path || !start_request_path || !start_response_path ||
        !start_receipt_path || !terminal_request_path ||
        !terminal_response_path || !terminal_receipt_path) return -1;

    int result = jef_verify_job_receipt(start_receipt_path, start_response_path,
                                       start_request_path, pubkey, "start", NULL);
    if (result != 0) return result;
    result = jef_verify_job_receipt(terminal_receipt_path, terminal_response_path,
                                    terminal_request_path, pubkey, "terminal", NULL);
    if (result != 0) return result;

    size_t spec_len = 0, request_len = 0, start_len = 0, terminal_len = 0;
    char* spec = v_read_alloc(spec_path, 1024 * 1024, &spec_len);
    char* request = v_read_alloc(start_request_path, 32 * 1024, &request_len);
    char* start = v_read_alloc(start_response_path, 600 * 1024, &start_len);
    char* terminal = v_read_alloc(terminal_response_path, 600 * 1024, &terminal_len);
    result = -1;
    if (!spec || !request || !start || !terminal) goto done;
    result = 1;
    if (!v_json_object_exact(request, request_len) ||
        !v_json_object_exact(start, start_len) ||
        !v_json_object_exact(terminal, terminal_len)) goto done;

    char expected_spec[72], got_spec[72] = "sha256:";
    jash_sha256(spec, spec_len, digest);
    jash_sha256_hex(digest, got_spec + 7);
    if (v_json_string(request, "\"execution_spec_sha256\"",
                      expected_spec, sizeof expected_spec) != 0 ||
        strcmp(expected_spec, got_spec) != 0) goto done;

    /* Both responses are already pinned and bound to their concrete receipts.
     * The terminal verifier's sibling start is not necessarily OUR start. */
    static const char* const start_keys[] = {
        "\"job_id\"", "\"receipt_ref\"", "\"receipt_blake3\"",
    };
    static const char* const terminal_keys[] = {
        "\"job_id\"", "\"start_receipt_ref\"", "\"start_receipt_blake3\"",
    };
    for (size_t i = 0; i < sizeof start_keys / sizeof start_keys[0]; i++) {
        char a[256], b[256];
        if (v_json_string(start, start_keys[i], a, sizeof a) != 0 ||
            v_json_string(terminal, terminal_keys[i], b, sizeof b) != 0 ||
            strcmp(a, b) != 0) goto done;
    }
    char start_request_hash[65], got[65];
    if (request_len && request[request_len - 1] == '\n') request_len--;
    jash_blake3(request, request_len, digest);
    jash_blake3_hex(digest, got);
    if (v_json_string(terminal, "\"start_request_blake3\"",
                      start_request_hash, sizeof start_request_hash) != 0 ||
        strcmp(start_request_hash, got) != 0) goto done;
    result = 0;
done:
    free(spec); free(request); free(start); free(terminal);
    return result;
}
