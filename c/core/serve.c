// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — core/serve — structured exec protocol daemon (ADR-0038 W6).
// See serve.h for the contract. F E R M I ∞ H A R T

#include "serve.h"
#include "mutation_report.h"
#include "agent_cancel.h"
#include "query_cancel.h"
#include <osif/osinfo.h>
#include <osif/ipc.h>
#include <osif/buffer.h>
#include "agent_contract.h"
#include <builtins/builtin.h>
#include <core/agent_process.h>
#include <core/env.h>
#include <core/execution_ir.h>
#include <core/agent_json.h>   /* jash_json_esc */
#include <core/fschange.h>
#include <core/output_budget.h> /* jash_tmp_path */
#include <core/provenance.h>
#include <module/modjash.h>
#include <module/jash_crypto.h>
#include <ed25519.h>
#include <jef_verify.h>
#include <parser/builtin_redir.h>
#include <parser/eval.h>
#include <theme/banner.h>       /* JASH_VERSION */

#include <fcntl.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/file.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/syscalls.h>

extern char** environ;

#define SRV_REQ_MAX      (256 * 1024)
#define SRV_ARGV_MAX     64
#define SRV_ARENA_MAX    (64 * 1024)
#define SRV_LINE_MAX     JASH_EXEC_JSON_LINE_MAX  /* uma definição só (serve.h) */
#define SRV_STDIN_MAX    (192 * 1024)
#define SRV_ENV_MAX      16
#define SRV_INLINE_MAX   (32 * 1024)   /* larger streams become refs */
#define SRV_AGENT_REQ_MAX (32 * 1024)
#define SRV_RESPONSE_MAX (512 * 1024)
#define SRV_IDEM_MAX     (600 * 1024)

/* All protocol writes share one delivery failure state. A failed response
 * closes the session before the next request can perform another effect. */
static int srv_response_fd = -1;
static int srv_response_failed = 0;
static int srv_write_all(int fd, const char* data, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, data + off, len - off);
        if (n > 0) { off += (size_t)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        if (fd == srv_response_fd) srv_response_failed = 1;
        return -1;
    }
    return 0;
}
static int srv_dprintf(int fd, const char* format, ...) __attribute__((format(printf, 2, 3)));
static int srv_dprintf(int fd, const char* format, ...) {
    va_list arguments, measured;
    va_start(arguments, format); va_copy(measured, arguments);
    int length = vsnprintf(NULL, 0, format, measured); va_end(measured);
    if (length < 0 || length > SRV_RESPONSE_MAX) {
        va_end(arguments); if (fd == srv_response_fd) srv_response_failed = 1; errno = EOVERFLOW; return -1;
    }
    char* bytes = malloc((size_t)length + 1);
    if (!bytes) { va_end(arguments); if (fd == srv_response_fd) srv_response_failed = 1; return -1; }
    int written = vsnprintf(bytes, (size_t)length + 1, format, arguments); va_end(arguments);
    if (written != length && fd == srv_response_fd) srv_response_failed = 1;
    int result = written != length || srv_write_all(fd, bytes, (size_t)length) ? -1 : length;
    free(bytes); return result;
}


#define SRV_JOB_MAX              32
#define SRV_JOB_WAIT_MAX_MS      30000L
#define SRV_JOB_GRACE_MAX_MS     5000L
#define SRV_JOB_READ_MAX         65536L
#define SRV_JOB_CAP_MAX          12
#define SRV_JOB_ID_MAX           40
#define SRV_JOB_CWD_MAX          JASH_AGENT_PROCESS_PATH_MAX
#define SRV_JOB_ENV_MAX          16
#define SRV_JOB_ENV_KEY_MAX      255u
#define SRV_JOB_ENV_VALUE_MAX    2047u
#define SRV_JOB_ENV_BYTES_MAX    (16u * 1024u)
#define SRV_JOB_STDIN_MAX        (16u * 1024u)
#define SRV_JOB_WRITE_MAX        (16u * 1024u)
#define SRV_STREAM_SLOTS         8
#define SRV_STREAM_WINDOW_MAX    65536u
#define SRV_STREAM_SESSION_MAX   (4u * SRV_STREAM_WINDOW_MAX)
#define SRV_DATA_MAX_BYTES       (16u * 1024u * 1024u)

#ifndef JASH_AGENT_RUNTIME_NAME
#define JASH_AGENT_RUNTIME_NAME "jash-agentd"
#endif

#ifndef JASH_GITHASH
#define JASH_GITHASH "unknown"
#endif

#ifdef JASH_AGENT_ONLY
static int srv_shutdown_fd = -1;
static int srv_shutdown_state; /* 1 requested by listener, 2 listener/control lost */
static int srv_store_lease = -1;
static int srv_data_fd = -1;
#define SRV_DATA_SLOTS 4
typedef struct { int used, fd; unsigned readers; uint64_t size; char ref[64], hash[65]; } srv_data_buffer_t;
static srv_data_buffer_t srv_data_buffers[SRV_DATA_SLOTS];
static uint64_t srv_data_sequence;
typedef struct { uint64_t max_bytes; int fd_enabled, explicit_policy; char revision[65]; } srv_data_policy_t;
static srv_data_buffer_t* srv_data_find(const char* ref);
static int srv_data_policy_load(srv_data_policy_t* policy);
static int store_cas_name(const char* name, char hash[65], uint64_t* size);
typedef struct {
    int used;
    char ref[64], job_id[SRV_JOB_ID_MAX], stream[8];
    uint64_t sequence, offset, acknowledged, credit, in_flight;
    int eof_sent;
} srv_data_stream_t;
static srv_data_stream_t srv_data_streams[SRV_STREAM_SLOTS];
static uint64_t srv_stream_sequence;
static uint64_t srv_stream_in_flight(void) {
    uint64_t bytes = 0;
    for (int i = 0; i < SRV_STREAM_SLOTS; i++)
        if (srv_data_streams[i].used) bytes += srv_data_streams[i].in_flight;
    return bytes;
}
static uint64_t srv_stream_credit_total(void) {
    uint64_t bytes = 0;
    for (int i = 0; i < SRV_STREAM_SLOTS; i++)
        if (srv_data_streams[i].used) bytes += srv_data_streams[i].credit;
    return bytes;
}
static uint64_t srv_data_bytes(void) { uint64_t bytes = 0; for (int i = 0; i < SRV_DATA_SLOTS; i++) if (srv_data_buffers[i].used) bytes += srv_data_buffers[i].size; return bytes; }
static void srv_data_close_child(void) {
    if (srv_data_fd >= 0) close(srv_data_fd);
    for (int i = 0; i < SRV_DATA_SLOTS; i++) if (srv_data_buffers[i].used) close(srv_data_buffers[i].fd);
}
static void srv_data_cleanup(void) {
    for (int i = 0; i < SRV_DATA_SLOTS; i++) if (srv_data_buffers[i].used) close(srv_data_buffers[i].fd);
    memset(srv_data_buffers, 0, sizeof(srv_data_buffers));
    memset(srv_data_streams, 0, sizeof(srv_data_streams));
}
static int srv_store_poisoned;
static char srv_store_home[768];
static const char* srv_home(void) { return srv_store_home[0] ? srv_store_home : getenv("HOME"); }

static int srv_shutdown_requested(void) {
    if (srv_shutdown_state || srv_shutdown_fd < 0) return srv_shutdown_state;
    struct pollfd control = {.fd = srv_shutdown_fd, .events = POLLIN};
    if (poll(&control, 1, 0) <= 0) return 0;
    char event;
    ssize_t n = recv(srv_shutdown_fd, &event, 1, MSG_DONTWAIT);
    if (n == 1) srv_shutdown_state = event == 'S' ? 1 : 2;
    else if (!n || (control.revents & (POLLHUP | POLLERR | POLLNVAL))) srv_shutdown_state = 2;
    return srv_shutdown_state;
}

/* Explicit buffering lets a managed worker poll its shutdown channel without
 * overlooking requests already prefetched from the data connection. */
typedef struct {
    int fd, idle_ms, error;
    size_t at, used;
    unsigned char bytes[4096];
} srv_managed_input_t;

static int srv_managed_getc(srv_managed_input_t* input) {
    if (srv_shutdown_state || input->error) return EOF;
    while (input->at == input->used) {
        struct pollfd inputs[2] = {{input->fd, POLLIN, 0}, {srv_shutdown_fd, POLLIN, 0}};
        int ready = poll(inputs, 2, input->idle_ms);
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) { input->error = 1; return EOF; }
        if (srv_shutdown_requested()) return EOF;
        ssize_t n = read(input->fd, input->bytes, sizeof(input->bytes));
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (n <= 0) { input->error = n < 0; return EOF; }
        input->at = 0; input->used = (size_t)n;
    }
    return input->bytes[input->at++];
}
#endif

static int srv_cloexec(int fd) {
    if (fd < 0) return -1;
    int flags = fcntl(fd, F_GETFD);
    if (flags < 0 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int srv_dup_cloexec(int fd) {
#ifdef F_DUPFD_CLOEXEC
    return fcntl(fd, F_DUPFD_CLOEXEC, 3);
#else
    return srv_cloexec(dup(fd));
#endif
}

/* ── request model ──────────────────────────────────────────────────────── */

typedef struct {
    char id[JASH_AGENT_ID_JSON_CAP]; /* raw JSON token, echoed back */
    int request_version;         /* absent means legacy v1 */
    int byte_strings;            /* tagged argv/cwd requires protocol v2 */
#ifdef JASH_AGENT_ONLY
    char id_value[JASH_AGENT_ID_VALUE_CAP]; /* decoded idempotency key */
    long timeout_ms;
    int cancelable;
#endif
    char* argv[SRV_ARGV_MAX];
    int   argc;
    char  arena[SRV_ARENA_MAX];
    unsigned arena_used;
    char* line;                  /* heap, may be NULL */
    char* stdin_buf;             /* heap, may be NULL */
    size_t stdin_len;
    char  cwd[1024];
    char  execution_spec_sha256[72];
    char* env_k[SRV_ENV_MAX];
    char* env_v[SRV_ENV_MAX];
    int   env_n;
#ifdef JASH_AGENT_ONLY
    char* caps[32];
    int   caps_n;
    int   caps_seen;
#endif
    unsigned seen;
} srv_req_t;

typedef struct {
    uint8_t pk[ED25519_PUBKEY];
    uint8_t sk[ED25519_SECKEY];
    char pubkey_hex[ED25519_PUBKEY * 2 + 1];
    int ready;
} srv_receipt_signer_t;

#ifdef JASH_AGENT_ONLY
typedef struct {
    char path[1024];
    char digest[JASH_BLAKE3_HEX_LEN];
    int fd;
} srv_idem_t;

typedef struct {
    char id[JASH_AGENT_ID_JSON_CAP];
    char id_value[JASH_AGENT_ID_VALUE_CAP];
    char op[32];
    char job_id[SRV_JOB_ID_MAX];
    char stream[8];
    char caps[SRV_JOB_CAP_MAX][32];
    int caps_n;
    int request_version;
    int background;
    int pty;
    unsigned rows;
    unsigned cols;
    long wait_ms;
    long grace_ms;
    long max_bytes, older_than_seconds;
    int dry_run;
    char root_namespace[32], root_name[256], root_blake3[65];
    char target_id[JASH_AGENT_ID_VALUE_CAP];
    char cursor[96];
    char revision[65], artifact_ref[160], transport[32];
    char stream_ref[64];
    int fd_enabled;
    long offset;
    long limit;
    long credit_bytes;
    long ack_offset;
    char* cwd;
    int cwd_fd;
    char resolved_cwd[SRV_JOB_CWD_MAX + 1u];
    char execution_spec_sha256[72];
    jash_agent_limits_t resources;
    char* env_k[SRV_JOB_ENV_MAX];
    char* env_v[SRV_JOB_ENV_MAX];
    int env_n;
    size_t env_bytes;
    uint8_t stdin_data[SRV_JOB_STDIN_MAX];
    size_t stdin_len;
    int stdin_pipe;
    uint8_t write_data[SRV_JOB_WRITE_MAX];
    size_t write_len;
    char request_digest[JASH_BLAKE3_HEX_LEN];
    uint64_t seen;
    jash_execution_ir_t ir;
    char arena[JASH_EXEC_IR_MAX_TOTAL_BYTES];
    size_t arena_used;
} srv_job_req_t;

typedef struct {
    int used;
    char job_id[SRV_JOB_ID_MAX];
    char request_id[JASH_AGENT_ID_VALUE_CAP];
    char request_digest[JASH_BLAKE3_HEX_LEN];
    char create_response[4096];
    char stdout_path[1100];
    char stderr_path[1100];
    char stdout_ref[160];
    char stderr_ref[160];
    char stdout_hash[JASH_BLAKE3_HEX_LEN];
    char stderr_hash[JASH_BLAKE3_HEX_LEN];
    unsigned long long stdout_bytes;
    unsigned long long stderr_bytes;
    dev_t stdout_dev;
    dev_t stderr_dev;
    ino_t stdout_ino;
    ino_t stderr_ino;
    char start_receipt_ref[160];
    char start_receipt_hash[JASH_BLAKE3_HEX_LEN];
    char receipt_pubkey[ED25519_PUBKEY * 2 + 1];
    int terminal_receipt_ready;
    int receipt_failed;
    int create_response_ready;
    int background;
    int pty;
    int stdin_pipe;
    int stdin_closed;
    int stdin_artifact;
    int stdin_stream;
    int stdin_artifact_pin;
    int stdin_artifact_durable;
    char stdin_artifact_ref[160];
    char stdin_stream_ref[64];
    char stdin_stream_final_ref[160];
    char stdin_stream_final_hash[JASH_BLAKE3_HEX_LEN];
    char stdin_artifact_hash[JASH_BLAKE3_HEX_LEN];
    uint64_t stdin_stream_offset;
    uint64_t stdin_artifact_bytes;
    uint64_t stdin_materialized_bytes;
    uint64_t stdin_verified_bytes;
    int cancel_attempted;
    int cancel_applied;
    int cancel_indeterminate;
    srv_idem_t reservation;
    jash_agent_process_t* process;
} srv_job_t;

typedef struct {
    srv_job_t jobs[SRV_JOB_MAX];
    int cgroup_parent_fd; /* Startup-pinned configuration, never a request field. */
    int input_fd, cancel_fd;
} srv_job_table_t;

static srv_data_stream_t* srv_stream_find(const char* ref);
static const char* srv_stream_source(srv_job_t* job, const char* stream,
                                     int verify_before_open,
                                     int* source_fd, uint64_t* size,
                                     int* terminal, unsigned* final_flags,
                                     const char** final_ref,
                                     const char** final_hash);

#endif


/* ── minimal JSON subset parser ─────────────────────────────────────────── */

typedef struct { const char* p; } jp_t;

static void jp_ws(jp_t* j) {
    while (*j->p == ' ' || *j->p == '\t' || *j->p == '\r' || *j->p == '\n')
        j->p++;
}

static int jp_hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int srv_sha256_identifier_valid(const char* value) {
    if (!value || strncmp(value, "sha256:", 7) != 0 || strlen(value) != 71)
        return 0;
    for (size_t i = 7; i < 71; i++) {
        if (!((value[i] >= '0' && value[i] <= '9') ||
              (value[i] >= 'a' && value[i] <= 'f')))
            return 0;
    }
    return 1;
}

static int jp_uint(jp_t* j, long max, long* out) {
    if (*j->p < '0' || *j->p > '9') return -1;
    long value = 0;
    do {
        int digit = *j->p - '0';
        if (value > (max - digit) / 10) return -1;
        value = value * 10 + digit;
        j->p++;
    } while (*j->p >= '0' && *j->p <= '9');
    *out = value;
    return 0;
}

#ifdef JASH_AGENT_ONLY
static int jp_bool(jp_t* j, int* out) {
    if (strncmp(j->p, "true", 4) == 0) {
        j->p += 4;
        *out = 1;
        return 0;
    }
    if (strncmp(j->p, "false", 5) == 0) {
        j->p += 5;
        *out = 0;
        return 0;
    }
    return -1;
}
#endif

static int srv_b64_value(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* Strict RFC 4648: padding is required, only allowed in the final quartet,
 * and unused bits must be zero. This rejects alternate encodings instead of
 * letting two request bodies denote the same binary stdin. */
static long srv_b64_decode(const char* in, size_t len, uint8_t* out,
                           size_t cap) {
    if (len % 4 != 0) return -1;
    size_t o = 0;
    for (size_t i = 0; i < len; i += 4) {
        int a = srv_b64_value((unsigned char)in[i]);
        int b = srv_b64_value((unsigned char)in[i + 1]);
        int c = in[i + 2] == '=' ? -2 :
            srv_b64_value((unsigned char)in[i + 2]);
        int d = in[i + 3] == '=' ? -2 :
            srv_b64_value((unsigned char)in[i + 3]);
        int final = i + 4 == len;
        if (a < 0 || b < 0 || c == -1 || d == -1) return -1;
        if (c == -2) {
            if (!final || d != -2 || (b & 15) != 0 || o + 1 > cap)
                return -1;
            out[o++] = (uint8_t)((a << 2) | (b >> 4));
        } else if (d == -2) {
            if (!final || (c & 3) != 0 || o + 2 > cap) return -1;
            out[o++] = (uint8_t)((a << 2) | (b >> 4));
            out[o++] = (uint8_t)((b << 4) | (c >> 2));
        } else {
            if (o + 3 > cap) return -1;
            out[o++] = (uint8_t)((a << 2) | (b >> 4));
            out[o++] = (uint8_t)((b << 4) | (c >> 2));
            out[o++] = (uint8_t)((c << 6) | d);
        }
    }
    return (long)o;
}

/* Starts at 'u', leaves the cursor on the last hex digit. */
static int jp_unicode(jp_t* j, unsigned* value) {
    unsigned cp = 0;
    for (int k = 0; k < 4; k++) {
        int h = jp_hex(j->p[1]);
        if (h < 0) return 0;
        j->p++; cp = (cp << 4) | (unsigned)h;
    }
    if (cp >= 0xd800 && cp <= 0xdbff) {
        if (j->p[1] != '\\' || j->p[2] != 'u') return 0;
        j->p += 2;
        unsigned low = 0;
        for (int k = 0; k < 4; k++) {
            int h = jp_hex(j->p[1]);
            if (h < 0) return 0;
            j->p++; low = (low << 4) | (unsigned)h;
        }
        if (low < 0xdc00 || low > 0xdfff) return 0;
        cp = 0x10000u + ((cp - 0xd800u) << 10) + low - 0xdc00u;
    } else if (cp >= 0xdc00 && cp <= 0xdfff) return 0;
    *value = cp;
    return 1;
}

/* Decode a JSON string into out (cap incl. NUL). Returns length or -1. */
static long jp_string(jp_t* j, char* out, size_t cap) {
    if (!out || cap == 0) return -1;
    if (*j->p != '"') return -1;
    j->p++;
    size_t o = 0;
    while (*j->p && *j->p != '"') {
        char c = *j->p;
        if ((unsigned char)c < 0x20) return -1;
        if ((unsigned char)c >= 0x80) {
            size_t span = jash_json_utf8_span((const unsigned char*)j->p);
            if (!span || span >= cap || o >= cap - span) return -1;
            memcpy(out + o, j->p, span);
            o += span; j->p += span;
            continue;
        }
        if (c == '\\' && j->p[1]) {
            j->p++;
            switch (*j->p) {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case 'r': c = '\r'; break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case '"': c = '"';  break;
                case '/': c = '/';  break;
                case '\\': c = '\\'; break;
                case 'u': {
                    unsigned cp = 0;
                    if (!jp_unicode(j, &cp) || cp == 0) return -1;
                    if (cp < 0x80) { c = (char)cp; }
                    else if (cp < 0x800) {
                        if (o + 2 >= cap) return -1;
                        out[o++] = (char)(0xC0 | (cp >> 6));
                        c = (char)(0x80 | (cp & 0x3F));
                    } else if (cp < 0x10000) {
                        if (o + 3 >= cap) return -1;
                        out[o++] = (char)(0xE0 | (cp >> 12));
                        out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        c = (char)(0x80 | (cp & 0x3F));
                    } else {
                        if (o + 4 >= cap) return -1;
                        out[o++] = (char)(0xF0 | (cp >> 18));
                        out[o++] = (char)(0x80 | ((cp >> 12) & 0x3F));
                        out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        c = (char)(0x80 | (cp & 0x3F));
                    }
                    break;
                }
                default: return -1;
            }
        }
        if (o + 1 >= cap) return -1;
        out[o++] = c;
        j->p++;
    }
    if (*j->p != '"') return -1;
    j->p++;
    out[o] = '\0';
    return (long)o;
}

/* Decode a JSON base64 string directly into the caller's arena. One quartet of
 * scratch space; no allocation proportional to encoded input. */
static long jp_b64_cstring(jp_t* j, char* out, size_t cap) {
    if (!out || !cap || *j->p != '"') return -1;
    j->p++;
    char quartet[4];
    size_t used = 0, qn = 0;
    while (*j->p && *j->p != '"') {
        unsigned c = (unsigned char)*j->p;
        if (c == '\\') {
            j->p++;
            if (*j->p == 'u') {
                if (!jp_unicode(j, &c) || c >= 128) return -1;
            } else if (*j->p == '/') c = '/';
            else return -1;
        }
        j->p++;
        quartet[qn++] = (char)c;
        if (qn == 4) {
            long n = srv_b64_decode(quartet, sizeof(quartet), (uint8_t*)out + used, cap - used - 1);
            if (n < 0) return -1;
            used += (size_t)n;
            if (quartet[2] == '=' || quartet[3] == '=') {
                if (*j->p != '"') return -1;
            }
            qn = 0;
        }
    }
    if (*j->p != '"' || qn || memchr(out, '\0', used)) return -1;
    j->p++;
    out[used] = '\0';
    return (long)used;
}

static long jp_byte_string(jp_t* j, char* out, size_t cap, int* tagged) {
    if (*j->p == '"') return jp_string(j, out, cap);
    if (!out || !cap || *j->p != '{') return -1;
    j->p++;
    unsigned seen = 0;
    long size = -1;
    for (;;) {
        jp_ws(j);
        if (*j->p == '}') { j->p++; break; }
        if (seen) {
            if (*j->p != ',') return -1;
            j->p++; jp_ws(j);
        }
        char key[16];
        if (jp_string(j, key, sizeof(key)) < 0) return -1;
        jp_ws(j);
        if (*j->p != ':') return -1;
        j->p++; jp_ws(j);
        if (!strcmp(key, "encoding")) {
            char value[16];
            if ((seen & 1) || jp_string(j, value, sizeof(value)) < 0 || strcmp(value, "base64")) return -1;
            seen |= 1;
        } else if (!strcmp(key, "data")) {
            if (seen & 2) return -1;
            size = jp_b64_cstring(j, out, cap);
            if (size < 0) return -1;
            seen |= 2;
        } else return -1;
    }
    if (seen != 3) return -1;
    if (tagged) *tagged = 1;
    return size;
}

/* Validate/skip without allocating. Used for operation probing and the legacy
 * reserved timeout field. Even ignored values must be well-formed JSON. */
static int jp_skip_string(jp_t* j) {
    if (*j->p != '"') return 0;
    j->p++;
    while (*j->p && *j->p != '"') {
        unsigned char c = (unsigned char)*j->p;
        if (c < 32) return 0;
        if (c == '\\') {
            j->p++;
            if (*j->p == 'u') {
                unsigned cp;
                if (!jp_unicode(j, &cp)) return 0;
            } else if (!*j->p || !strchr("\"\\/bfnrt", *j->p)) return 0;
            j->p++;
        } else if (c >= 128) {
            size_t n = jash_json_utf8_span((const unsigned char*)j->p);
            if (!n) return 0;
            j->p += n;
        } else j->p++;
    }
    if (*j->p != '"') return 0;
    j->p++;
    return 1;
}

static int jp_scalar(jp_t* j) {
    const char* p = j->p;
    if (!strncmp(p, "true", 4)) p += 4;
    else if (!strncmp(p, "false", 5)) p += 5;
    else if (!strncmp(p, "null", 4)) p += 4;
    else {
        if (*p == '-') p++;
        if (*p == '0') p++;
        else {
            if (*p < '1' || *p > '9') return 0;
            do { p++; } while (*p >= '0' && *p <= '9');
        }
        if (*p == '.') {
            p++;
            if (*p < '0' || *p > '9') return 0;
            do { p++; } while (*p >= '0' && *p <= '9');
        }
        if (*p == 'e' || *p == 'E') {
            p++;
            if (*p == '+' || *p == '-') p++;
            if (*p < '0' || *p > '9') return 0;
            do { p++; } while (*p >= '0' && *p <= '9');
        }
    }
    if (*p && !strchr(",}] \t\r\n", *p)) return 0;
    j->p = p;
    return 1;
}

static int jp_skip_depth(jp_t* j, unsigned depth) {
    if (depth > 64) return 0;
    jp_ws(j);
    char c = *j->p;
    if (c == '"') return jp_skip_string(j);
    if (c == '{' || c == '[') {
        char close = (c == '{') ? '}' : ']';
        j->p++;
        jp_ws(j);
        if (*j->p == close) { j->p++; return 1; }
        for (;;) {
            jp_ws(j);
            if (!*j->p) return 0;
            if (c == '{') {                 /* key : value */
                /* Probing must not impose a smaller key limit than parsing. */
                if (!jp_skip_string(j)) return 0;
                jp_ws(j);
                if (*j->p != ':') return 0;
                j->p++;
            }
            if (!jp_skip_depth(j, depth + 1)) return 0;
            jp_ws(j);
            if (*j->p == close) { j->p++; return 1; }
            if (*j->p != ',') return 0;
            j->p++;
        }
    }
    return jp_scalar(j);
}

static int jp_skip(jp_t* j) { return jp_skip_depth(j, 0); }

#ifdef JASH_AGENT_ONLY
static int srv_probe_job_op(const char* text) {
    jp_t j = {text};
    jp_ws(&j);
    if (*j.p++ != '{') return 0;
    int first = 1;
    int has_op = 0;
    for (;;) {
        jp_ws(&j);
        if (*j.p == '}') return has_op;
        if (!first) {
            if (*j.p++ != ',') return 0;
            jp_ws(&j);
        }
        first = 0;
        char key[64];
        if (jp_string(&j, key, sizeof key) < 0) return 0;
        jp_ws(&j);
        if (*j.p++ != ':') return 0;
        jp_ws(&j);
        if (strcmp(key, "op") == 0) {
            char op[32];
            if (jp_string(&j, op, sizeof op) < 0) return 0;
            has_op = 1;
        } else if (!jp_skip(&j)) {
            return 0;
        }
    }
}

static const char* srv_job_string(jp_t* j, srv_job_req_t* request,
                                  char** out) {
    if (request->arena_used >= sizeof request->arena) return "IR string arena full";
    char* destination = request->arena + request->arena_used;
    long length = jp_string(j, destination,
                            sizeof request->arena - request->arena_used);
    if (length < 0) return "IR string is invalid or too long";
    request->arena_used += (size_t)length + 1u;
    *out = destination;
    return NULL;
}

static const char* srv_job_byte_string(jp_t* j, srv_job_req_t* request, char** out) {
    if (request->arena_used >= sizeof(request->arena)) return "IR string arena full";
    char* destination = request->arena + request->arena_used;
    long n = jp_byte_string(j, destination, sizeof(request->arena) - request->arena_used, NULL);
    if (n < 0) return "invalid byte string or capacity exceeded";
    request->arena_used += (size_t)n + 1;
    *out = destination;
    return NULL;
}

static const char* srv_job_parse_argv(jp_t* j, srv_job_req_t* request,
                                      jash_execution_stage_t* stage) {
    if (*j->p++ != '[') return "argv must be an array";
    int first = 1;
    for (;;) {
        jp_ws(j);
        if (*j->p == ']') { j->p++; break; }
        if (!first) {
            if (*j->p++ != ',') return "argv missing ','";
            jp_ws(j);
            if (*j->p == ']') return "trailing comma in argv";
        } else if (*j->p == ',') {
            return "leading comma in argv";
        }
        first = 0;
        if (stage->argc >= JASH_EXEC_IR_MAX_ARGV) return "argv too long";
        char* value = NULL;
        const char* error = srv_job_byte_string(j, request, &value);
        if (error) return error;
        stage->argv[stage->argc++] = value;
    }
    return stage->argc ? NULL : "argv must not be empty";
}

static const char* srv_job_parse_redirection(jp_t* j,
                                              srv_job_req_t* request,
                                              jash_execution_redirection_t* out) {
    if (*j->p++ != '{') return "redirection must be an object";
    unsigned seen = 0;
    int first = 1;
    char kind[16] = "";
    for (;;) {
        jp_ws(j);
        if (*j->p == '}') { j->p++; break; }
        if (!first) {
            if (*j->p++ != ',') return "redirection missing ','";
            jp_ws(j);
            if (*j->p == '}') return "trailing comma in redirection";
        }
        first = 0;
        char key[32];
        if (jp_string(j, key, sizeof key) < 0) return "bad redirection key";
        jp_ws(j);
        if (*j->p++ != ':') return "redirection missing ':'";
        jp_ws(j);
        if (strcmp(key, "fd") == 0) {
            if (seen & 1u) return "duplicate redirection fd";
            seen |= 1u;
            long fd = 0;
            if (jp_uint(j, 2, &fd) != 0) return "redirection fd must be 0..2";
            out->fd = (unsigned)fd;
        } else if (strcmp(key, "kind") == 0) {
            if (seen & 2u) return "duplicate redirection kind";
            seen |= 2u;
            if (jp_string(j, kind, sizeof kind) < 0)
                return "redirection kind must be a string";
        } else if (strcmp(key, "path") == 0) {
            if (seen & 4u) return "duplicate redirection path";
            seen |= 4u;
            char* path = NULL;
            const char* error = srv_job_byte_string(j, request, &path);
            if (error) return error;
            out->path = path;
        } else {
            return "unknown redirection field";
        }
    }
    if (seen != 7u) return "redirection requires fd, kind, and path";
    if (strcmp(kind, "read") == 0) out->kind = JASH_EXEC_REDIR_READ;
    else if (strcmp(kind, "truncate") == 0)
        out->kind = JASH_EXEC_REDIR_WRITE_TRUNC;
    else if (strcmp(kind, "append") == 0)
        out->kind = JASH_EXEC_REDIR_WRITE_APPEND;
    else return "redirection kind must be read, truncate, or append";
    return NULL;
}

static const char* srv_job_parse_redirections(jp_t* j,
                                               srv_job_req_t* request,
                                               jash_execution_stage_t* stage) {
    if (*j->p++ != '[') return "redirs must be an array";
    int first = 1;
    for (;;) {
        jp_ws(j);
        if (*j->p == ']') { j->p++; break; }
        if (!first) {
            if (*j->p++ != ',') return "redirs missing ','";
            jp_ws(j);
            if (*j->p == ']') return "trailing comma in redirs";
        }
        first = 0;
        if (stage->redirection_count >= JASH_EXEC_IR_MAX_REDIRS)
            return "too many redirections";
        const char* error = srv_job_parse_redirection(
            j, request, &stage->redirections[stage->redirection_count]);
        if (error) return error;
        stage->redirection_count++;
    }
    return NULL;
}

static const char* srv_job_parse_stage(jp_t* j, srv_job_req_t* request,
                                       jash_execution_stage_t* stage) {
    if (*j->p++ != '{') return "pipeline stage must be an object";
    unsigned seen = 0;
    int first = 1;
    for (;;) {
        jp_ws(j);
        if (*j->p == '}') { j->p++; break; }
        if (!first) {
            if (*j->p++ != ',') return "stage missing ','";
            jp_ws(j);
            if (*j->p == '}') return "trailing comma in stage";
        }
        first = 0;
        char key[32];
        if (jp_string(j, key, sizeof key) < 0) return "bad stage key";
        jp_ws(j);
        if (*j->p++ != ':') return "stage missing ':'";
        jp_ws(j);
        if (strcmp(key, "argv") == 0) {
            if (seen & 1u) return "duplicate stage argv";
            seen |= 1u;
            const char* error = srv_job_parse_argv(j, request, stage);
            if (error) return error;
        } else if (strcmp(key, "redirs") == 0) {
            if (seen & 2u) return "duplicate stage redirs";
            seen |= 2u;
            const char* error = srv_job_parse_redirections(j, request, stage);
            if (error) return error;
        } else {
            return "unknown pipeline stage field";
        }
    }
    return (seen & 1u) ? NULL : "pipeline stage requires argv";
}

static const char* srv_job_parse_ir(jp_t* j, srv_job_req_t* request) {
    if (*j->p++ != '{') return "ir must be an object";
    jash_execution_ir_init(&request->ir);
    unsigned seen = 0;
    int first = 1;
    for (;;) {
        jp_ws(j);
        if (*j->p == '}') { j->p++; break; }
        if (!first) {
            if (*j->p++ != ',') return "ir missing ','";
            jp_ws(j);
            if (*j->p == '}') return "trailing comma in ir";
        }
        first = 0;
        char key[32];
        if (jp_string(j, key, sizeof key) < 0) return "bad ir key";
        jp_ws(j);
        if (*j->p++ != ':') return "ir missing ':'";
        jp_ws(j);
        if (strcmp(key, "version") == 0) {
            if (seen & 1u) return "duplicate ir version";
            seen |= 1u;
            long version = 0;
            if (jp_uint(j, UINT_MAX, &version) != 0)
                return "ir version must be an integer";
            request->ir.version = (unsigned)version;
        } else if (strcmp(key, "pipeline") == 0) {
            if (seen & 2u) return "duplicate ir pipeline";
            seen |= 2u;
            if (*j->p++ != '[') return "pipeline must be an array";
            int pipeline_first = 1;
            for (;;) {
                jp_ws(j);
                if (*j->p == ']') { j->p++; break; }
                if (!pipeline_first) {
                    if (*j->p++ != ',') return "pipeline missing ','";
                    jp_ws(j);
                    if (*j->p == ']') return "trailing comma in pipeline";
                }
                pipeline_first = 0;
                if (request->ir.stage_count >= JASH_EXEC_IR_MAX_STAGES)
                    return "too many pipeline stages";
                const char* error = srv_job_parse_stage(
                    j, request, &request->ir.stages[request->ir.stage_count]);
                if (error) return error;
                request->ir.stage_count++;
            }
        } else {
            return "unknown ir field";
        }
    }
    return seen == 3u ? NULL : "ir requires version and pipeline";
}

static const char* srv_job_parse_caps(jp_t* j, srv_job_req_t* request) {
    if (*j->p++ != '[') return "caps must be an array";
    int first = 1;
    for (;;) {
        jp_ws(j);
        if (*j->p == ']') { j->p++; break; }
        if (!first) {
            if (*j->p++ != ',') return "caps missing ','";
            jp_ws(j);
            if (*j->p == ']') return "trailing comma in caps";
        }
        first = 0;
        if (request->caps_n >= SRV_JOB_CAP_MAX) return "too many capabilities";
        if (jp_string(j, request->caps[request->caps_n],
                      sizeof request->caps[0]) < 0)
            return "capability must be a short string";
        for (int i = 0; i < request->caps_n; i++)
            if (strcmp(request->caps[i], request->caps[request->caps_n]) == 0)
                return "duplicate capability";
        request->caps_n++;
    }
    return request->caps_n ? NULL : "capabilities required";
}

static int srv_job_env_name_valid(const char* name) {
    const unsigned char* p = (const unsigned char*)name;
    if (!(*p == '_' || isalpha(*p))) return 0;
    for (p++; *p; p++)
        if (!(*p == '_' || isalnum(*p))) return 0;
    return 1;
}

static int srv_env_protected(const char* name) {
    return strcmp(name, "HOME") == 0 ||
           strcmp(name, "PWD") == 0 ||
           strcmp(name, "JASH_AGENT_CANCEL_FD") == 0 ||
           strcmp(name, "JASH_AGENT_CGROUP_ROOT") == 0 ||
           strncmp(name, "LD_", 3) == 0 ||
           strncmp(name, "DYLD_", 5) == 0;
}

static const char* srv_job_parse_env(jp_t* j, srv_job_req_t* request) {
    if (*j->p++ != '{') return "env must be an object";
    int first = 1;
    for (;;) {
        jp_ws(j);
        if (*j->p == '}') { j->p++; break; }
        if (!first) {
            if (*j->p++ != ',') return "env missing ','";
            jp_ws(j);
            if (*j->p == '}') return "trailing comma in env";
        }
        first = 0;
        if (request->env_n >= SRV_JOB_ENV_MAX) return "env has too many entries";
        char* key = NULL;
        char* value = NULL;
        const char* error = srv_job_string(j, request, &key);
        if (error) return "env key is invalid or too long";
        jp_ws(j);
        if (*j->p++ != ':') return "env missing ':'";
        jp_ws(j);
        error = srv_job_string(j, request, &value);
        if (error) return "env value is invalid or too long";
        size_t key_len = strlen(key), value_len = strlen(value);
        if (key_len > SRV_JOB_ENV_KEY_MAX || !srv_job_env_name_valid(key))
            return "env key must be a valid short name";
        if (value_len > SRV_JOB_ENV_VALUE_MAX)
            return "env value exceeds its byte limit";
        if (srv_env_protected(key)) return "protected env cannot be overridden";
        for (int i = 0; i < request->env_n; i++)
            if (strcmp(request->env_k[i], key) == 0)
                return "duplicate env key";
        size_t entry_bytes = key_len + 1u + value_len + 1u;
        if (request->env_bytes > SRV_JOB_ENV_BYTES_MAX - entry_bytes)
            return "env exceeds its byte limit";
        request->env_bytes += entry_bytes;
        request->env_k[request->env_n] = key;
        request->env_v[request->env_n] = value;
        request->env_n++;
    }
    return NULL;
}

static const char* srv_job_parse_b64(jp_t* j, srv_job_req_t* request,
                                     uint8_t* out, size_t cap,
                                     size_t* decoded, const char* field) {
    char* encoded = NULL;
    const char* error = srv_job_string(j, request, &encoded);
    if (error) return strcmp(field, "stdin_b64") == 0
        ? "stdin_b64 is invalid or too long" : "data_b64 is invalid or too long";
    long length = srv_b64_decode(encoded, strlen(encoded), out, cap);
    if (length < 0) return strcmp(field, "stdin_b64") == 0
        ? "bad or oversized stdin_b64" : "bad or oversized data_b64";
    *decoded = (size_t)length;
    return NULL;
}

static const char* srv_job_parse_resources(jp_t* j, jash_agent_limits_t* limits) {
    static const char* const names[] = {"memory_max_bytes", "pids_max", "cpu_quota_us",
                                       "cpu_period_us", "wall_timeout_ms"};
    uint64_t* values[] = {&limits->memory_max_bytes, &limits->pids_max, &limits->cpu_quota_us,
                         &limits->cpu_period_us, &limits->wall_timeout_ms};
    if (*j->p++ != '{') return "resources must be an object";
    unsigned seen = 0;
    for (;;) {
        jp_ws(j);
        if (*j->p == '}') { j->p++; break; }
        if (seen) {
            if (*j->p++ != ',') return "resources missing ','";
            jp_ws(j);
        }
        char name[64];
        if (jp_string(j, name, sizeof name) < 0) return "invalid resource key";
        unsigned field;
        for (field = 0; field < 5 && strcmp(name, names[field]); field++) {}
        if (field == 5 || (seen & (1u << field))) return "unknown or duplicate resource field";
        jp_ws(j);
        if (*j->p++ != ':') return "resources missing ':'";
        jp_ws(j);
        if (*j->p < '0' || *j->p > '9' ||
            (*j->p == '0' && j->p[1] >= '0' && j->p[1] <= '9'))
            return "resource values must be unsigned integers";
        uint64_t value = 0;
        do {
            unsigned digit = (unsigned)(*j->p++ - '0');
            if (value > (UINT64_MAX - digit) / 10) return "resource integer overflow";
            value = value * 10 + digit;
        } while (*j->p >= '0' && *j->p <= '9');
        *values[field] = value;
        seen |= 1u << field;
    }
    if (seen != 31u) return "resources requires all five limits";
    if (jash_agent_limits_validate(limits, NULL, 0) != 0)
        return "resources outside supported bounds";
    return NULL;
}

static const char* srv_job_parse(const char* text, srv_job_req_t* request) {
    memset(request, 0, sizeof *request);
    snprintf(request->id, sizeof request->id, "null");
    request->request_version = 1;
    request->cwd_fd = -1;
    request->rows = 24;
    request->cols = 80;
    request->wait_ms = 5000;
    request->grace_ms = JASH_AGENT_PROCESS_DEFAULT_GRACE_MS;
    request->older_than_seconds = 3600;
    request->limit = SRV_JOB_READ_MAX;
    jash_execution_ir_init(&request->ir);

    jp_t j = {text};
    jp_ws(&j);
    if (*j.p++ != '{') return "request must be a JSON object";
    int first = 1;
    for (;;) {
        jp_ws(&j);
        if (*j.p == '}') { j.p++; break; }
        if (!first) {
            if (*j.p++ != ',') return "request missing ','";
            jp_ws(&j);
            if (*j.p == '}') return "trailing comma in request";
        }
        first = 0;
        char key[64];
        if (jp_string(&j, key, sizeof key) < 0) return "bad request key";
        jp_ws(&j);
        if (*j.p++ != ':') return "request missing ':'";
        jp_ws(&j);

#define JOB_DUP(bit, message) do { \
    if (request->seen & (bit)) return (message); \
    request->seen |= (bit); \
} while (0)
        if (strcmp(key, "id") == 0) {
            JOB_DUP(SRV_JOB_SEEN_ID, "duplicate id");
            if (jp_string(&j, request->id_value, sizeof request->id_value) < 0 ||
                !request->id_value[0]) return "id must be a non-empty string";
            char escaped[JASH_AGENT_ID_JSON_CAP];
            jash_json_esc(request->id_value, escaped, sizeof escaped);
            size_t id_len = strlen(escaped);
            if (id_len > sizeof request->id - 3) return "encoded id too long";
            request->id[0] = '"';
            memcpy(request->id + 1, escaped, id_len);
            memcpy(request->id + id_len + 1, "\"", 2);
        } else if (strcmp(key, "protocol_version") == 0 ||
                   strcmp(key, "request_version") == 0) {
            JOB_DUP(SRV_JOB_SEEN_VERSION, "duplicate protocol version");
            long version = 0;
            if (jp_uint(&j, 2, &version) != 0 || version != 2)
                return "job operations require protocol version 2";
            request->request_version = 2;
        } else if (strcmp(key, "op") == 0) {
            JOB_DUP(SRV_JOB_SEEN_OP, "duplicate op");
            if (jp_string(&j, request->op, sizeof request->op) < 0)
                return "op must be a short string";
        } else if (strcmp(key, "caps") == 0) {
            JOB_DUP(SRV_JOB_SEEN_CAPS, "duplicate caps");
            const char* error = srv_job_parse_caps(&j, request);
            if (error) return error;
        } else if (strcmp(key, "ir") == 0) {
            JOB_DUP(SRV_JOB_SEEN_IR, "duplicate ir");
            if (request->seen & SRV_JOB_SEEN_ARGV) return "ir and argv are exclusive";
            const char* error = srv_job_parse_ir(&j, request);
            if (error) return error;
        } else if (strcmp(key, "argv") == 0) {
            JOB_DUP(SRV_JOB_SEEN_ARGV, "duplicate argv");
            if (request->seen & SRV_JOB_SEEN_IR) return "ir and argv are exclusive";
            request->ir.stage_count = 1;
            const char* error = srv_job_parse_argv(&j, request,
                                                   &request->ir.stages[0]);
            if (error) return error;
        } else if (strcmp(key, "background") == 0) {
            JOB_DUP(SRV_JOB_SEEN_BACKGROUND, "duplicate background");
            if (jp_bool(&j, &request->background) != 0)
                return "background must be boolean";
        } else if (strcmp(key, "pty") == 0) {
            JOB_DUP(SRV_JOB_SEEN_PTY, "duplicate pty");
            if (jp_bool(&j, &request->pty) != 0) return "pty must be boolean";
        } else if (strcmp(key, "rows") == 0) {
            JOB_DUP(SRV_JOB_SEEN_ROWS, "duplicate rows");
            long value = 0;
            if (jp_uint(&j, 65535, &value) != 0 || value < 1)
                return "rows must be in 1..65535";
            request->rows = (unsigned)value;
        } else if (strcmp(key, "cols") == 0) {
            JOB_DUP(SRV_JOB_SEEN_COLS, "duplicate cols");
            long value = 0;
            if (jp_uint(&j, 65535, &value) != 0 || value < 1)
                return "cols must be in 1..65535";
            request->cols = (unsigned)value;
        } else if (strcmp(key, "job_id") == 0) {
            JOB_DUP(SRV_JOB_SEEN_JOB_ID, "duplicate job_id");
            if (jp_string(&j, request->job_id, sizeof request->job_id) < 0 ||
                !request->job_id[0]) return "job_id must be a short string";
        } else if (strcmp(key, "wait_ms") == 0) {
            JOB_DUP(SRV_JOB_SEEN_WAIT, "duplicate wait_ms");
            if (jp_uint(&j, SRV_JOB_WAIT_MAX_MS, &request->wait_ms) != 0)
                return "wait_ms must be in 0..30000";
        } else if (strcmp(key, "grace_ms") == 0) {
            JOB_DUP(SRV_JOB_SEEN_GRACE, "duplicate grace_ms");
            if (jp_uint(&j, SRV_JOB_GRACE_MAX_MS, &request->grace_ms) != 0)
                return "grace_ms must be in 0..5000";
        } else if (strcmp(key, "max_bytes") == 0) {
            JOB_DUP(SRV_JOB_SEEN_MAX_BYTES, "duplicate max_bytes");
            if (jp_uint(&j, LONG_MAX, &request->max_bytes)) return "invalid max_bytes";
        } else if (strcmp(key, "older_than_seconds") == 0) {
            JOB_DUP(SRV_JOB_SEEN_AGE, "duplicate older_than_seconds");
            if (jp_uint(&j, LONG_MAX, &request->older_than_seconds)) return "invalid older_than_seconds";
        } else if (strcmp(key, "dry_run") == 0) {
            JOB_DUP(SRV_JOB_SEEN_DRY_RUN, "duplicate dry_run");
            if (jp_bool(&j, &request->dry_run)) return "invalid dry_run";
        } else if (!strcmp(key, "revision")) {
            JOB_DUP(SRV_JOB_SEEN_REVISION, "duplicate revision");
            if (jp_string(&j, request->revision, sizeof(request->revision)) < 0) return "invalid revision";
        } else if (!strcmp(key, "fd_enabled")) {
            JOB_DUP(SRV_JOB_SEEN_FD_ENABLED, "duplicate fd_enabled");
            if (jp_bool(&j, &request->fd_enabled)) return "invalid fd_enabled";
        } else if (!strcmp(key, "artifact_ref")) {
            JOB_DUP(SRV_JOB_SEEN_ARTIFACT_REF, "duplicate artifact_ref");
            if (jp_string(&j, request->artifact_ref, sizeof(request->artifact_ref)) < 0) return "invalid artifact_ref";
        } else if (!strcmp(key, "transport")) {
            JOB_DUP(SRV_JOB_SEEN_TRANSPORT, "duplicate transport");
            if (jp_string(&j, request->transport, sizeof(request->transport)) < 0) return "invalid transport";
        } else if (!strcmp(key, "stream_ref")) {
            JOB_DUP(SRV_JOB_SEEN_STREAM_REF, "duplicate stream_ref");
            if (jp_string(&j, request->stream_ref, sizeof request->stream_ref) < 0 ||
                !request->stream_ref[0]) return "invalid stream_ref";
        } else if (!strcmp(key, "credit_bytes")) {
            JOB_DUP(SRV_JOB_SEEN_CREDIT_BYTES, "duplicate credit_bytes");
            if (jp_uint(&j, SRV_STREAM_WINDOW_MAX, &request->credit_bytes))
                return "credit_bytes must be in 0..65536";
        } else if (!strcmp(key, "ack_offset")) {
            JOB_DUP(SRV_JOB_SEEN_ACK_OFFSET, "duplicate ack_offset");
            if (jp_uint(&j, LONG_MAX, &request->ack_offset))
                return "ack_offset must be a non-negative integer";
        } else if (!strcmp(key, "cursor")) {
            JOB_DUP(SRV_JOB_SEEN_CURSOR, "duplicate cursor");
            if (jp_string(&j, request->cursor, sizeof(request->cursor)) < 0) return "invalid cursor";
        } else if (!strcmp(key, "target_id")) {
            JOB_DUP(SRV_JOB_SEEN_TARGET_ID, "duplicate target_id");
            if (jp_string(&j, request->target_id, sizeof(request->target_id)) < 0 || !request->target_id[0]) return "invalid target_id";
        } else if (!strcmp(key, "root_namespace")) {
            JOB_DUP(SRV_JOB_SEEN_ROOT_NS, "duplicate root_namespace");
            if (jp_string(&j, request->root_namespace, sizeof(request->root_namespace)) < 0) return "invalid root_namespace";
        } else if (!strcmp(key, "root_name")) {
            JOB_DUP(SRV_JOB_SEEN_ROOT_NAME, "duplicate root_name");
            if (jp_string(&j, request->root_name, sizeof(request->root_name)) < 0) return "invalid root_name";
        } else if (!strcmp(key, "root_blake3")) {
            JOB_DUP(SRV_JOB_SEEN_ROOT_HASH, "duplicate root_blake3");
            if (jp_string(&j, request->root_blake3, sizeof(request->root_blake3)) < 0) return "invalid root_blake3";
        } else if (strcmp(key, "stream") == 0) {
            JOB_DUP(SRV_JOB_SEEN_STREAM, "duplicate stream");
            if (jp_string(&j, request->stream, sizeof request->stream) < 0)
                return "stream must be stdout or stderr";
        } else if (strcmp(key, "offset") == 0) {
            JOB_DUP(SRV_JOB_SEEN_OFFSET, "duplicate offset");
            if (jp_uint(&j, LONG_MAX, &request->offset) != 0)
                return "offset must be a non-negative integer";
        } else if (strcmp(key, "limit") == 0) {
            JOB_DUP(SRV_JOB_SEEN_LIMIT, "duplicate limit");
            if (jp_uint(&j, SRV_JOB_READ_MAX, &request->limit) != 0 ||
                request->limit < 1) return "limit must be in 1..65536";
        } else if (strcmp(key, "cwd") == 0) {
            JOB_DUP(SRV_JOB_SEEN_CWD, "duplicate cwd");
            const char* error = srv_job_byte_string(&j, request, &request->cwd);
            if (error || !request->cwd[0] ||
                strlen(request->cwd) > SRV_JOB_CWD_MAX)
                return "cwd must be a non-empty path within its byte limit";
        } else if (strcmp(key, "execution_spec_sha256") == 0) {
            JOB_DUP(SRV_JOB_SEEN_EXECUTION_SPEC,
                    "duplicate execution_spec_sha256");
            if (jp_string(&j, request->execution_spec_sha256,
                          sizeof request->execution_spec_sha256) < 0 ||
                !srv_sha256_identifier_valid(request->execution_spec_sha256))
                return "execution_spec_sha256 must be lowercase sha256:<64 hex>";
        } else if (strcmp(key, "resources") == 0) {
            JOB_DUP(SRV_JOB_SEEN_RESOURCES, "duplicate resources");
            const char* error = srv_job_parse_resources(&j, &request->resources);
            if (error) return error;
        } else if (strcmp(key, "env") == 0) {
            JOB_DUP(SRV_JOB_SEEN_ENV, "duplicate env");
            const char* error = srv_job_parse_env(&j, request);
            if (error) return error;
        } else if (strcmp(key, "stdin_b64") == 0) {
            JOB_DUP(SRV_JOB_SEEN_STDIN_B64, "duplicate stdin_b64");
            const char* error = srv_job_parse_b64(
                &j, request, request->stdin_data, sizeof request->stdin_data,
                &request->stdin_len, "stdin_b64");
            if (error) return error;
        } else if (strcmp(key, "stdin_mode") == 0) {
            JOB_DUP(SRV_JOB_SEEN_STDIN_MODE, "duplicate stdin_mode");
            char mode[8];
            if (jp_string(&j, mode, sizeof mode) < 0 ||
                strcmp(mode, "pipe") != 0)
                return "stdin_mode must be pipe";
            request->stdin_pipe = 1;
        } else if (strcmp(key, "data_b64") == 0) {
            JOB_DUP(SRV_JOB_SEEN_DATA_B64, "duplicate data_b64");
            const char* error = srv_job_parse_b64(
                &j, request, request->write_data, sizeof request->write_data,
                &request->write_len, "data_b64");
            if (error) return error;
        } else {
            return "unknown job request field";
        }
#undef JOB_DUP
    }
    jp_ws(&j);
    if (*j.p) return "trailing data after request";
    return NULL;
}

static int srv_job_has_cap(const srv_job_req_t* request, const char* cap) {
    for (int i = 0; i < request->caps_n; i++)
        if (strcmp(request->caps[i], cap) == 0) return 1;
    return 0;
}

static const char* srv_job_validate_cwd(srv_job_req_t* request) {
    if (!(request->seen & SRV_JOB_SEEN_CWD)) return NULL;
#if !defined(O_DIRECTORY) || !defined(O_NOFOLLOW) || !defined(O_CLOEXEC)
    return "secure cwd opening is unsupported";
#else
    int fd = open(request->cwd,
                  O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return "cwd is not an accessible directory";
    struct stat metadata;
    if (fstat(fd, &metadata) != 0 || !S_ISDIR(metadata.st_mode)) {
        close(fd);
        return "cwd is not an accessible directory";
    }
#ifdef __linux__
    char proc_path[64];
    int pn = snprintf(proc_path, sizeof proc_path, "/proc/self/fd/%d", fd);
    ssize_t length = pn > 0 && (size_t)pn < sizeof proc_path
        ? readlink(proc_path, request->resolved_cwd, SRV_JOB_CWD_MAX) : -1;
    if (length <= 0 || length > SRV_JOB_CWD_MAX) {
        close(fd);
        return "cwd canonical path is unavailable";
    }
    request->resolved_cwd[length] = '\0';
#elif defined(F_GETPATH)
    if (fcntl(fd, F_GETPATH, request->resolved_cwd) != 0) {
        close(fd);
        return "cwd canonical path is unavailable";
    }
#else
    close(fd);
    return "cwd canonical path is unavailable";
#endif
    if (request->resolved_cwd[0] != '/') {
        close(fd);
        return "cwd canonical path is unavailable";
    }
    request->cwd_fd = fd;
    return NULL;
#endif
}

static const char* srv_job_validate(srv_job_req_t* request) {
    const jash_job_contract_t* contract = jash_job_contract_find(request->op);
    const char* field_error = jash_job_contract_fields(contract, request->seen);
    if (field_error) return field_error;
    static const char* known_caps[] = {
        "effect:exec", "job:create", "job:read", "job:signal",
        "job:stdin:write", "job:pty:create", "job:pty:resize",
        "job:pty:write",
        "store:read", "store:manage", "data:create", "data:read", "data:manage",
    };
    for (int i = 0; i < request->caps_n; i++) {
        int known = 0;
        for (size_t k = 0; k < sizeof known_caps / sizeof known_caps[0]; k++)
            if (strcmp(request->caps[i], known_caps[k]) == 0) known = 1;
        if (!known) return "unknown job capability";
    }

    if (strcmp(request->op, "exec") == 0 ||
        strcmp(request->op, "job.start") == 0) {
        int start = strcmp(request->op, "job.start") == 0;
        if (!(request->seen & (SRV_JOB_SEEN_IR | SRV_JOB_SEEN_ARGV)))
            return "exec and job.start require ir or argv";
        if (!srv_job_has_cap(request, "effect:exec") ||
            !srv_job_has_cap(request, "job:create"))
            return "create operation requires effect:exec and job:create";
        if (start && (!(request->seen & SRV_JOB_SEEN_BACKGROUND) ||
                      !request->background))
            return "job.start requires background:true";
        if (request->pty && !srv_job_has_cap(request, "job:pty:create"))
            return "PTY start requires job:pty:create";
        if ((request->seen & (SRV_JOB_SEEN_ROWS | SRV_JOB_SEEN_COLS)) &&
            !request->pty) return "rows and cols require pty:true";
        if (request->pty && (request->seen & SRV_JOB_SEEN_STDIN_B64))
            return "PTY start rejects stdin_b64; use job.write";
        if (request->stdin_pipe && request->pty)
            return "stdin_mode pipe rejects pty:true";
        if (request->stdin_pipe && (request->seen & SRV_JOB_SEEN_STDIN_B64))
            return "stdin_mode pipe rejects stdin_b64";
        int artifact_input = !!(request->seen & SRV_JOB_SEEN_ARTIFACT_REF);
        int stream_input = !!(request->seen & SRV_JOB_SEEN_STREAM_REF);
        if (artifact_input && stream_input)
            return "artifact_ref and stream_ref are exclusive";
        if (artifact_input || stream_input) {
            if (artifact_input && !srv_job_has_cap(request, "data:read"))
                return "artifact stdin requires data:read";
            if (stream_input && !srv_job_has_cap(request, "job:read"))
                return "stream stdin requires job:read";
            if (request->seen & SRV_JOB_SEEN_STDIN_B64)
                return "reference stdin and stdin_b64 are exclusive";
            if (request->seen & SRV_JOB_SEEN_STDIN_MODE)
                return "reference stdin and stdin_mode are exclusive";
            if (request->pty) return "reference stdin rejects pty:true";
            for (size_t i = 0; i < request->ir.stages[0].redirection_count; i++)
                if (request->ir.stages[0].redirections[i].fd == 0)
                    return "reference stdin rejects stage-zero stdin redirection";
        }
        jash_execution_ir_error_t ir_error;
        if (jash_execution_ir_validate(&request->ir, &ir_error) != 0)
            return jash_execution_ir_error_string(ir_error);
        return srv_job_validate_cwd(request);
    }

    const char* needed_cap = contract->capability;
    if (!strcmp(request->op, "data.policy.set")) {
        if (strlen(request->revision) != 64) return "invalid revision";
        for (int i = 0; i < 64; i++) if (!strchr("0123456789abcdef", request->revision[i])) return "invalid revision";
    }
    if ((request->seen & SRV_JOB_SEEN_ARTIFACT_REF) && !request->artifact_ref[0]) return "invalid artifact_ref";
    if (!strcmp(request->op, "data.import")) {
        if (!strcmp(request->transport, "inline")) { if (!(request->seen & SRV_JOB_SEEN_DATA_B64)) return "inline requires data_b64"; }
        else if (!strcmp(request->transport, "sealed-fd")) { if (request->seen & SRV_JOB_SEEN_DATA_B64) return "FD import rejects inline data"; }
        else return "transport_unsupported";
    }
    if (!strcmp(request->op, "data.read") && (request->limit < 1 || request->limit > 65536)) return "read limit must be 1..65536";
    if (!strcmp(request->op, "store.history")) {
        if (!(request->seen & SRV_JOB_SEEN_LIMIT)) request->limit = 16;
        if (request->limit < 1 || request->limit > 64) return "history limit must be 1..64";
        if ((request->seen & SRV_JOB_SEEN_CURSOR) && !request->cursor[0]) return "invalid cursor";
    }
    if (!strcmp(request->op, "store.release")) {
        if ((strcmp(request->root_namespace, "agent_job_results") && strcmp(request->root_namespace, "agent_receipts") &&
             strcmp(request->root_namespace, "agent_requests")) || !request->root_name[0] || request->root_name[0] == '.' ||
            strchr(request->root_name, '/') || strlen(request->root_blake3) != 64) return "invalid release root";
        for (int i = 0; i < 64; i++) if (!strchr("0123456789abcdef", request->root_blake3[i])) return "invalid release hash";
    }
    if (strcmp(request->op, "job.read") == 0 ||
        strcmp(request->op, "data.stream.open") == 0) {
        if (strcmp(request->stream, "stdout") != 0 &&
            strcmp(request->stream, "stderr") != 0)
            return "stream must be stdout or stderr";
    } else if (strcmp(request->op, "job.write") == 0) {
        if (!srv_job_has_cap(request, "job:pty:write") &&
            !srv_job_has_cap(request, "job:stdin:write"))
            return "capability denied";
    }
    return !needed_cap || srv_job_has_cap(request, needed_cap)
        ? NULL : "capability denied";
}
#endif

/* Parse one request object. Returns NULL on success, error string on fail. */
static const char* srv_parse(const char* text, srv_req_t* r) {
    memset(r, 0, sizeof(*r));
    snprintf(r->id, sizeof(r->id), "null");
    r->request_version = 1;
#ifdef JASH_AGENT_ONLY
    r->timeout_ms = -1;
#endif
    jp_t j = { text };
    jp_ws(&j);
    if (*j.p != '{') return "request must be a JSON object";
    j.p++;
    int object_first = 1;
    for (;;) {
        jp_ws(&j);
        if (*j.p == '}') { j.p++; break; }
        if (!object_first) {
            if (*j.p != ',') return "missing ',' between fields";
            j.p++;
            jp_ws(&j);
            if (*j.p == '}') return "trailing comma in object";
        } else if (*j.p == ',') {
            return "leading comma in object";
        }
        object_first = 0;
        char key[64];
        if (jp_string(&j, key, sizeof(key)) < 0) return "bad key";
        jp_ws(&j);
        if (*j.p != ':') return "missing ':'";
        j.p++;
        jp_ws(&j);

        if (strcmp(key, "id") == 0) {
            if (r->seen & SRV_SEEN_ID) return "duplicate id";
            r->seen |= SRV_SEEN_ID;
#ifdef JASH_AGENT_ONLY
            if (*j.p != '"') return "id must be a string";
#endif
            if (*j.p == '"') {
                char idv[JASH_AGENT_ID_VALUE_CAP];
                if (jp_string(&j, idv, sizeof(idv)) < 0) return "bad id";
#ifdef JASH_AGENT_ONLY
                if (!idv[0]) return "id must not be empty";
                snprintf(r->id_value, sizeof(r->id_value), "%s", idv);
#endif
                char esc[JASH_AGENT_ID_JSON_CAP];
                jash_json_esc(idv, esc, sizeof(esc));
                size_t id_len = strlen(esc);
                if (id_len > sizeof r->id - 3) return "encoded id too long";
                r->id[0] = '"';
                memcpy(r->id + 1, esc, id_len);
                memcpy(r->id + id_len + 1, "\"", 2);
            } else {
                const char* start = j.p;
                if (!jp_skip(&j)) return "bad id";
                size_t n = (size_t)(j.p - start);
                if (!n || n >= sizeof(r->id)) return "bad id";
                memcpy(r->id, start, n);
                r->id[n] = '\0';
            }
        } else if (strcmp(key, "argv") == 0) {
            if (r->seen & SRV_SEEN_ARGV) return "duplicate argv";
            r->seen |= SRV_SEEN_ARGV;
            if (*j.p != '[') return "argv must be an array";
            j.p++;
            int argv_first = 1;
            for (;;) {
                jp_ws(&j);
                if (*j.p == ']') { j.p++; break; }
                if (!argv_first) {
                    if (*j.p != ',') return "argv missing ','";
                    j.p++;
                    jp_ws(&j);
                    if (*j.p == ']') return "trailing comma in argv";
                } else if (*j.p == ',') {
                    return "leading comma in argv";
                }
                argv_first = 0;
                if (r->argc >= SRV_ARGV_MAX) return "argv too long";
                if (r->arena_used >= SRV_ARENA_MAX) return "argv arena full";
                char* dst = r->arena + r->arena_used;
                long n = jp_byte_string(&j, dst,
                                       SRV_ARENA_MAX - r->arena_used, &r->byte_strings);
                if (n < 0) return "argv element must be a string";
                r->argv[r->argc++] = dst;
                r->arena_used += (unsigned)n + 1;
            }
        } else if (strcmp(key, "line") == 0) {
            if (r->seen & SRV_SEEN_LINE) return "duplicate line";
            r->seen |= SRV_SEEN_LINE;
            r->line = (char*)malloc(SRV_LINE_MAX);
            if (!r->line) return "oom";
            if (jp_string(&j, r->line, SRV_LINE_MAX) < 0)
                return "bad line";
        } else if (strcmp(key, "stdin") == 0) {
            if (r->seen & SRV_SEEN_STDIN) return "duplicate stdin";
            if (r->seen & SRV_SEEN_STDIN_B64)
                return "stdin and stdin_b64 are exclusive";
            r->seen |= SRV_SEEN_STDIN;
            r->stdin_buf = (char*)malloc(SRV_STDIN_MAX);
            if (!r->stdin_buf) return "oom";
            long n = jp_string(&j, r->stdin_buf, SRV_STDIN_MAX);
            if (n < 0) return "bad stdin";
            r->stdin_len = (size_t)n;
        } else if (strcmp(key, "stdin_b64") == 0) {
            if (r->seen & SRV_SEEN_STDIN_B64) return "duplicate stdin_b64";
            if (r->seen & SRV_SEEN_STDIN)
                return "stdin and stdin_b64 are exclusive";
            r->seen |= SRV_SEEN_STDIN_B64;
            char* encoded = (char*)malloc(SRV_REQ_MAX + 1);
            r->stdin_buf = (char*)malloc(SRV_STDIN_MAX);
            if (!encoded || !r->stdin_buf) {
                free(encoded);
                return "oom";
            }
            long encoded_len = jp_string(&j, encoded, SRV_REQ_MAX + 1);
            long decoded_len = encoded_len < 0 ? -1 :
                srv_b64_decode(encoded, (size_t)encoded_len,
                               (uint8_t*)r->stdin_buf, SRV_STDIN_MAX - 1);
            free(encoded);
            if (decoded_len < 0) return "bad stdin_b64";
            r->stdin_len = (size_t)decoded_len;
        } else if (strcmp(key, "request_version") == 0 ||
                   strcmp(key, "protocol_version") == 0) {
            if (r->seen & SRV_SEEN_VERSION)
                return "duplicate request protocol version";
            r->seen |= SRV_SEEN_VERSION;
            long version = 0;
            if (jp_uint(&j, 2, &version) != 0 || version < 1)
                return "protocol version must be 1 or 2";
            r->request_version = (int)version;
        } else if (strcmp(key, "timeout_ms") == 0) {
            if (r->seen & SRV_SEEN_TIMEOUT) return "duplicate timeout_ms";
            r->seen |= SRV_SEEN_TIMEOUT;
#ifdef JASH_AGENT_ONLY
            if (jp_uint(&j, 600000, &r->timeout_ms) != 0 || r->timeout_ms < 1)
                return "timeout_ms must be an integer from 1 to 600000";
#else
            /* JINV-044: aceito e RESERVADO (serve.h) — nao imposto em v1.
             * Fica explicito aqui para nao cair no ramo de chave desconhecida;
             * `--capabilities-json` ja o declara como "reserved". */
            if (!jp_skip(&j)) return "malformed value";
#endif
        } else if (strcmp(key, "cwd") == 0) {
            if (r->seen & SRV_SEEN_CWD) return "duplicate cwd";
            r->seen |= SRV_SEEN_CWD;
            if (jp_byte_string(&j, r->cwd, sizeof(r->cwd), &r->byte_strings) < 0) return "bad cwd";
        } else if (strcmp(key, "execution_spec_sha256") == 0) {
            if (r->seen & SRV_SEEN_EXECUTION_SPEC)
                return "duplicate execution_spec_sha256";
            r->seen |= SRV_SEEN_EXECUTION_SPEC;
            if (jp_string(&j, r->execution_spec_sha256,
                          sizeof(r->execution_spec_sha256)) < 0 ||
                !srv_sha256_identifier_valid(r->execution_spec_sha256))
                return "execution_spec_sha256 must be lowercase sha256:<64 hex>";
#ifdef JASH_AGENT_ONLY
        } else if (strcmp(key, "cancelable") == 0) {
            if (r->seen & SRV_SEEN_CANCELABLE) return "duplicate cancelable";
            r->seen |= SRV_SEEN_CANCELABLE;
            if (jp_bool(&j, &r->cancelable) != 0)
                return "cancelable must be boolean";
        } else if (strcmp(key, "caps") == 0) {
            if (r->seen & SRV_SEEN_CAPS) return "duplicate caps";
            r->seen |= SRV_SEEN_CAPS;
            r->caps_seen = 1;
            if (*j.p != '[') return "caps must be an array";
            j.p++;
            int caps_first = 1;
            for (;;) {
                jp_ws(&j);
                if (*j.p == ']') { j.p++; break; }
                if (!caps_first) {
                    if (*j.p != ',') return "caps missing ','";
                    j.p++;
                    jp_ws(&j);
                    if (*j.p == ']') return "trailing comma in caps";
                } else if (*j.p == ',') {
                    return "leading comma in caps";
                }
                caps_first = 0;
                if (r->caps_n >= 32) return "caps too long";
                if (r->arena_used >= SRV_ARENA_MAX) return "caps arena full";
                char* dst = r->arena + r->arena_used;
                long n = jp_string(&j, dst, SRV_ARENA_MAX - r->arena_used);
                if (n < 0) return "caps element must be a string";
                r->caps[r->caps_n++] = dst;
                r->arena_used += (unsigned)n + 1;
            }
#endif
        } else if (strcmp(key, "env") == 0) {
            if (r->seen & SRV_SEEN_ENV) return "duplicate env";
            r->seen |= SRV_SEEN_ENV;
            if (*j.p != '{') return "env must be an object";
            j.p++;
            int env_first = 1;
            for (;;) {
                jp_ws(&j);
                if (*j.p == '}') { j.p++; break; }
                if (!env_first) {
                    if (*j.p != ',') return "env missing ','";
                    j.p++;
                    jp_ws(&j);
                    if (*j.p == '}') return "trailing comma in env";
                } else if (*j.p == ',') {
                    return "leading comma in env";
                }
                env_first = 0;
                /* The synchronous overlay must round-trip through state.h's
                 * env table; a truncated key cannot be found during restore. */
                char ek[JASH_ENV_KEY_LEN], ev[2048];
                if (jp_string(&j, ek, sizeof(ek)) < 0) return "bad env key";
                if (!ek[0] || strchr(ek, '=')) return "invalid env key";
                jp_ws(&j);
                if (*j.p != ':') return "env missing ':'";
                j.p++;
                jp_ws(&j);
                if (jp_string(&j, ev, sizeof(ev)) < 0) return "bad env value";
#ifdef JASH_AGENT_ONLY
                if (srv_env_protected(ek)) return "protected env cannot be overridden";
#endif
                if (r->env_n >= SRV_ENV_MAX) return "env too long";
                for (int i = 0; i < r->env_n; i++) {
                    if (strcmp(r->env_k[i], ek) == 0)
                        return "duplicate env key";
                }
                char* env_k = strdup(ek);
                char* env_v = strdup(ev);
                if (!env_k || !env_v) {
                    free(env_k);
                    free(env_v);
                    return "oom";
                }
                r->env_k[r->env_n] = env_k;
                r->env_v[r->env_n] = env_v;
                r->env_n++;
            }
        } else {
            /* JINV-044: chave desconhecida e ERRO, nao ruido a ignorar.
             *
             * Ate aqui, `{"argv":["echo","x"],"stdim":"typo"}` executava e
             * devolvia 0 — o autor pensava ter passado stdin e nao passou. Um
             * typo num campo vira um pedido silenciosamente diferente do que
             * foi escrito.
             *
             * Isto e a MESMA raiz do JINV-017 (cwd/env/stdin aceitos e
             * ignorados): aceitar-e-ignorar. Um erro alto custa um segundo; um
             * campo engolido custa a depuracao inteira, longe da causa.
             *
             * `--serve` compartilha este parser, entao a recusa vale para as
             * duas portas — que e o ponto do JINV-017. */
            static char unknown_msg[160];
            snprintf(unknown_msg, sizeof unknown_msg,
                     "chave desconhecida '%s' (see --contract-json invocation:serve)", key);
            return unknown_msg;
        }
    }
    jp_ws(&j);
    if (*j.p) return "trailing data after request";
    if (r->byte_strings && r->request_version != 2) return "byte strings require protocol version 2";
    return jash_sync_contract_fields(r->seen, r->argc, r->request_version);
}

static void srv_req_free(srv_req_t* r) {
    free(r->line);
    free(r->stdin_buf);
    for (int i = 0; i < r->env_n; i++) { free(r->env_k[i]); free(r->env_v[i]); }
}

static int srv_hash_file(const char* path, char out[JASH_BLAKE3_HEX_LEN]) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    struct blake3 hash;
    blake3_init(&hash);
    uint8_t buf[8192];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n > 0) { blake3_update(&hash, buf, (uint64_t)n); continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { close(fd); return -1; }
        break;
    }
    close(fd);
    uint8_t digest[32];
    blake3_final(&hash, digest, sizeof digest);
    jash_blake3_hex(digest, out);
    return 0;
}

#ifdef JASH_AGENT_ONLY
enum {
    SRV_IDEM_NEW = 0,
    SRV_IDEM_REPLAY = 1,
    SRV_IDEM_CONFLICT = 2,
    SRV_IDEM_INDETERMINATE = 3,
    SRV_IDEM_ERROR = 4,
};

static int srv_idem_appended_replay(char* frame, char* limit, char** replay) {
    static const char version[] = "completed-v1\n";
    size_t available = (size_t)(limit - frame);
    if (available < sizeof(version) - 1 ||
        memcmp(frame, version, sizeof(version) - 1) != 0)
        return SRV_IDEM_INDETERMINATE;

    char* length = frame + sizeof(version) - 1;
    char* length_end = memchr(length, '\n', (size_t)(limit - length));
    if (!length_end || length_end == length)
        return SRV_IDEM_INDETERMINATE;
    for (char* p = length; p < length_end; p++)
        if (!isdigit((unsigned char)*p)) return SRV_IDEM_INDETERMINATE;
    *length_end = '\0';
    errno = 0;
    char* number_end = NULL;
    unsigned long long declared = strtoull(length, &number_end, 10);
    if (errno == ERANGE || !number_end || *number_end ||
        declared == 0 || declared > SRV_RESPONSE_MAX)
        return SRV_IDEM_INDETERMINATE;

    char* stored_hash = length_end + 1;
    char* hash_end = memchr(stored_hash, '\n', (size_t)(limit - stored_hash));
    if (!hash_end || (size_t)(hash_end - stored_hash) !=
                         JASH_BLAKE3_HEX_LEN - 1)
        return SRV_IDEM_INDETERMINATE;
    *hash_end = '\0';

    char* response = hash_end + 1;
    size_t response_len = (size_t)(limit - response);
    if (response_len != (size_t)declared || response[response_len - 1] != '\n' ||
        memchr(response, '\0', response_len))
        return SRV_IDEM_INDETERMINATE;

    uint8_t digest[32];
    char observed_hash[JASH_BLAKE3_HEX_LEN];
    jash_blake3(response, response_len, digest);
    jash_blake3_hex(digest, observed_hash);
    if (strcmp(stored_hash, observed_hash) != 0)
        return SRV_IDEM_INDETERMINATE;

    *replay = (char*)malloc(response_len + 1);
    if (!*replay) return SRV_IDEM_ERROR;
    memcpy(*replay, response, response_len);
    (*replay)[response_len] = '\0';
    return SRV_IDEM_REPLAY;
}

static int srv_secure_private_dir(const char* path) {
    struct stat named, opened, after;
    if (lstat(path, &named) != 0 || !S_ISDIR(named.st_mode) ||
        S_ISLNK(named.st_mode) || named.st_uid != geteuid())
        return -1;
    int flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
#ifdef O_DIRECTORY
    flags |= O_DIRECTORY;
#endif
    int fd = open(path, flags);
    if (fd < 0 || fstat(fd, &opened) != 0 || !S_ISDIR(opened.st_mode) ||
        opened.st_uid != geteuid() || opened.st_dev != named.st_dev ||
        opened.st_ino != named.st_ino || fchmod(fd, 0700) != 0 ||
        fstat(fd, &after) != 0 || after.st_dev != opened.st_dev ||
        after.st_ino != opened.st_ino || (after.st_mode & 0777) != 0700 ||
        lstat(path, &named) != 0 || named.st_dev != after.st_dev ||
        named.st_ino != after.st_ino || (named.st_mode & 0777) != 0700) {
        if (fd >= 0) close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

static int srv_private_dir(const char* leaf, char* out, size_t cap) {
    const char* home = srv_home();
    if (!home || !*home) return -1;
    char root[768];
    int rn = snprintf(root, sizeof(root), "%s/.jash", home);
    int dn = snprintf(out, cap, "%s/.jash/%s", home, leaf);
    if (rn <= 0 || (size_t)rn >= sizeof(root) || dn <= 0 || (size_t)dn >= cap)
        return -1;
    if (mkdir(root, 0700) != 0 && errno != EEXIST) return -1;
    if (srv_secure_private_dir(root) != 0) return -1;
    if (mkdir(out, 0700) != 0 && errno != EEXIST) return -1;
    return srv_secure_private_dir(out);
}

static int srv_fsync_dir(const char* path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    int rc = fsync(fd);
    close(fd);
    return rc;
}

static int srv_regular_identity_ok(const char* path,
                                   const struct stat* opened) {
    struct stat named;
    return opened && S_ISREG(opened->st_mode) &&
           opened->st_uid == geteuid() && (opened->st_mode & 0777) == 0600 &&
           opened->st_nlink == 1 && lstat(path, &named) == 0 &&
           S_ISREG(named.st_mode) &&
           !S_ISLNK(named.st_mode) && named.st_uid == geteuid() &&
           (named.st_mode & 0777) == 0600 && named.st_nlink == 1 &&
           opened->st_dev == named.st_dev && opened->st_ino == named.st_ino;
}

static void srv_unlink_if_identity(const char* path,
                                   const struct stat* opened) {
    struct stat named;
    if (opened && lstat(path, &named) == 0 &&
        named.st_dev == opened->st_dev && named.st_ino == opened->st_ino)
        (void)unlink(path);
}

static int srv_open_regular_nofollow(const char* path, struct stat* opened) {
    struct stat named;
    if (lstat(path, &named) != 0 || !S_ISREG(named.st_mode) ||
        S_ISLNK(named.st_mode))
        return -1;
    int flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    int fd = open(path, flags);
    if (fd < 0 || fstat(fd, opened) != 0 || !S_ISREG(opened->st_mode) ||
        opened->st_uid != geteuid() || (opened->st_mode & 0777) != 0600 ||
        opened->st_nlink != 1 || opened->st_dev != named.st_dev ||
        opened->st_ino != named.st_ino) {
        if (fd >= 0) close(fd);
        return -1;
    }
    return fd;
}

#define SRV_STORE_MAX_FILES 4096u
#define SRV_STORE_SCAN_BYTES (64u * 1024u * 1024u)

static int srv_store_lock_file(const char* name) {
    char dir[896], path[1100];
    if (srv_private_dir("agent_store", dir, sizeof(dir))) return -1;
    int n = snprintf(path, sizeof(path), "%s/%s", dir, name);
    if (n < 0 || (size_t)n >= sizeof(path)) return -1;
    int fd = open(path, O_RDWR | O_CREAT | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600);
    struct stat st;
    if (fd < 0) return -1;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() || st.st_nlink != 1 ||
        (st.st_mode & 0077) || fchmod(fd, 0600)) { close(fd); return -1; }
    return fd;
}

static int srv_store_bind(void) {
    const char* home = getenv("HOME"); char resolved[PATH_MAX];
    if (srv_store_lease >= 0 || !home || !realpath(home, resolved) || strlen(resolved) >= sizeof(srv_store_home)) return -1;
    /* INV-JASH-TIER1-SOURCE-001: retain the admitted, complete home name. */
    memcpy(srv_store_home, resolved, strlen(resolved) + 1);
    int fd = srv_store_lock_file("lease");
    if (fd < 0 || flock(fd, LOCK_SH)) { if (fd >= 0) close(fd); srv_store_home[0] = 0; return -1; }
    srv_store_lease = fd; return 0;
}

static int srv_store_dir(const char* name) {
    char path[1100]; const char* home = srv_home();
    int n = home ? snprintf(path, sizeof(path), "%s/.jash/%s", home, name) : -1;
    if (n < 0 || (size_t)n >= sizeof(path)) return -1;
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    struct stat st;
    if (fd >= 0 && (fstat(fd, &st) || st.st_uid != geteuid() || (st.st_mode & 0022))) { close(fd); errno = EPERM; return -1; }
    return fd;
}

static int srv_store_policy_limit(const char* name, uint64_t* limit) {
    *limit = 0;
    int dir = srv_store_dir("agent_store"); if (dir < 0) return -1;
    int fd = openat(dir, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    int saved = errno; close(dir);
    if (fd < 0) return saved == ENOENT ? 0 : -1;
    struct stat st; char bytes[32];
    int ok = !fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_uid == geteuid() && st.st_nlink == 1 &&
        !(st.st_mode & 0077) && st.st_size > 0 && st.st_size < (off_t)sizeof(bytes);
    ssize_t n = ok ? read(fd, bytes, sizeof(bytes)) : -1; close(fd);
    if (!ok || n != st.st_size || bytes[n - 1] != '\n') return -1;
    uint64_t value = 0;
    if (n == 1) return -1;
    for (ssize_t i = 0; i < n - 1; i++) {
        if (bytes[i] < '0' || bytes[i] > '9' || value > ((uint64_t)LONG_MAX - (unsigned)(bytes[i] - '0')) / 10) return -1;
        value = value * 10 + (unsigned)(bytes[i] - '0');
    }
    *limit = value; return 0;
}

static int srv_store_limit(uint64_t* limit) { return srv_store_policy_limit("quota", limit); }

static const char* srv_store_policy_publish(const char* name, uint64_t value, int* applied) {
    int dir = srv_store_dir("agent_store"); char temporary[96], bytes[32];
    snprintf(temporary, sizeof(temporary), ".%s-%ld.tmp", name, (long)getpid());
    int length = snprintf(bytes, sizeof(bytes), "%llu\n", (unsigned long long)value);
    int fd = dir >= 0 ? openat(dir, temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600) : -1;
    int good = fd >= 0 && !fchmod(fd, 0600) && !srv_write_all(fd, bytes, (size_t)length) && !fsync(fd);
    if (fd >= 0 && close(fd)) good = 0;
    const char* error = NULL;
    if (good && !renameat(dir, temporary, dir, name)) { *applied = 1; if (fsync(dir)) error = "quota_sync_failed"; }
    else error = "quota_publish_failed";
    if (!*applied && fd >= 0) (void)unlinkat(dir, temporary, 0);
    if (dir >= 0) close(dir);
    return error;
}

static int srv_store_usage(uint64_t* used) {
    static const char* const names[] = {"overflow", "agent_job_results"};
    *used = 0; unsigned count = 0;
    for (size_t i = 0; i < 2; i++) {
        int fd = srv_store_dir(names[i]);
        if (fd < 0) { if (errno == ENOENT) continue; return -1; }
        DIR* directory = fdopendir(fd); if (!directory) { close(fd); return -1; }
        int failed = 0;
        for (;;) {
            errno = 0; struct dirent* entry = readdir(directory);
            if (!entry) { failed = errno != 0; break; }
            if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
            if (i == 0 && strncmp(entry->d_name, "blake3-", 7) && strncmp(entry->d_name, ".cas-", 5) && strncmp(entry->d_name, ".gc-", 4)) continue;
            struct stat st;
            if (++count > SRV_STORE_MAX_FILES || fstatat(fd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) ||
                !S_ISREG(st.st_mode) || st.st_size < 0 || UINT64_MAX - *used < (uint64_t)st.st_size) { failed = 1; break; }
            *used += (uint64_t)st.st_size;
        }
        closedir(directory); if (failed) return -1;
    }
    return 0;
}

/* The session lease excludes policy updates. Quota-enabled publishers serialize
 * the complete admission/publish operation; zero remains unlimited. */
static int srv_store_admit(uint64_t additional, const char* dedup_path, int* lock) {
    *lock = -1;
    if (srv_store_lease < 0) return 0; /* standalone internal fixtures */
    uint64_t limit, used;
    if (srv_store_limit(&limit)) return -1;
    if (!limit) return 0;
    *lock = srv_store_lock_file("admission");
    if (*lock < 0 || flock(*lock, LOCK_EX)) {
        if (*lock >= 0) close(*lock);
        *lock = -1; return -1;
    }
    struct stat existing;
    /* Recheck after taking admission: a concurrent publisher may have won
     * since the caller first inspected the name. The publisher verifies bytes. */
    if (dedup_path && !lstat(dedup_path, &existing)) additional = 0;
    if (srv_store_usage(&used) || used > limit || additional > limit - used) {
        if (*lock >= 0) close(*lock);
        *lock = -1; errno = EDQUOT; return -1;
    }
    return 0;
}

static int srv_hash_regular_fd(int fd, off_t expected_size,
                               char out[JASH_BLAKE3_HEX_LEN]) {
    struct blake3 hash;
    blake3_init(&hash);
    uint8_t buf[8192];
    off_t total = 0;
    for (;;) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n > 0) {
            if (total > expected_size - n) return -1;
            total += n;
            blake3_update(&hash, buf, (uint64_t)n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 || total != expected_size) return -1;
        break;
    }
    uint8_t digest[32];
    blake3_final(&hash, digest, sizeof digest);
    jash_blake3_hex(digest, out);
    return 0;
}

typedef struct {
    struct blake3 full,range;
    uint64_t offset,length;
} srv_stream_hash_t;
static int srv_stream_hash_chunk(void *user,const void *bytes,size_t n,uint64_t at) {
    srv_stream_hash_t *h=user;
    uint64_t first=at>h->offset?at:h->offset;
    uint64_t last=at+n<h->offset+h->length?at+n:h->offset+h->length;
    blake3_update(&h->full,bytes,n);
    if(first<last)blake3_update(&h->range,(const uint8_t *)bytes+(size_t)(first-at),last-first);
    return 0;
}

static int srv_cas_verify(const char* path, off_t expected_size,
                          const char* expected_hash) {
    struct stat st, after;
    int fd = srv_open_regular_nofollow(path, &st);
    if (fd < 0 || st.st_size != expected_size) {
        if (fd >= 0) close(fd);
        return -1;
    }
    char observed[JASH_BLAKE3_HEX_LEN];
    int ok = srv_hash_regular_fd(fd, expected_size, observed) == 0 &&
             strcmp(observed, expected_hash) == 0 &&
             fstat(fd, &after) == 0 && after.st_dev == st.st_dev &&
             after.st_ino == st.st_ino && after.st_size == st.st_size &&
             after.st_nlink == 1 && srv_regular_identity_ok(path, &after);
    close(fd);
    return ok ? 0 : -1;
}

/* Publish immutable output without replacing an existing name. EEXIST is a
 * dedup hit only when the pinned regular file still has the promised bytes. */
static int srv_cas_publish_impl(const char* source, off_t size, const char* hash,
                           char* ref, size_t refcap) {
    char dir[896];
    if (srv_private_dir("overflow", dir, sizeof dir) != 0) return -1;
    int rn = snprintf(ref, refcap, "blake3-%s-%lld.bin", hash,
                      (long long)size);
    if (rn <= 0 || (size_t)rn >= refcap) return -1;
    char final[1100];
    int fn = snprintf(final, sizeof final, "%s/%s", dir, ref);
    if (fn <= 0 || (size_t)fn >= sizeof final) return -1;

    int flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    struct stat existing;
    if (!lstat(final, &existing)) {
        if (srv_cas_verify(final, size, hash)) return -1;
        return srv_fsync_dir(dir);
    }
    if (errno != ENOENT) return -1;
    static unsigned serial;
    char temporary[1100];
    int tn = snprintf(temporary, sizeof(temporary), "%s/.cas-%s-%ld-%u.tmp", dir, hash, (long)getpid(), ++serial);
    if (tn < 0 || (size_t)tn >= sizeof(temporary)) return -1;
    int out = open(temporary, flags, 0600);
    if (out < 0) return -1;

    struct stat source_st, output_st = {0}, output_after;
    int owned = fstat(out, &output_st) == 0 && S_ISREG(output_st.st_mode);
    int in = srv_open_regular_nofollow(source, &source_st);
    int ok = owned && in >= 0 && source_st.st_size == size;
    struct blake3 copied;
    blake3_init(&copied);
    off_t total = 0;
    uint8_t buf[8192];
    while (ok) {
        ssize_t n = read(in, buf, sizeof buf);
        if (n > 0) {
            if (total > size - n ||
                srv_write_all(out, (const char*)buf, (size_t)n) != 0) {
                ok = 0;
                break;
            }
            total += n;
            blake3_update(&copied, buf, (uint64_t)n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) ok = 0;
        break;
    }
    if (in >= 0) close(in);
    uint8_t digest[32];
    char observed[JASH_BLAKE3_HEX_LEN];
    blake3_final(&copied, digest, sizeof digest);
    jash_blake3_hex(digest, observed);
    if (total != size || strcmp(observed, hash) != 0 || fsync(out) != 0 ||
        fstat(out, &output_after) != 0 || output_after.st_uid != geteuid() ||
        (output_after.st_mode & 0777) != 0600 || output_after.st_nlink != 1 ||
        output_after.st_dev != output_st.st_dev ||
        output_after.st_ino != output_st.st_ino ||
        !srv_regular_identity_ok(temporary, &output_after))
        ok = 0;
    if (close(out)) ok = 0;
    if (!ok) {
        srv_unlink_if_identity(temporary, &output_st);
        (void)srv_fsync_dir(dir);
        return -1;
    }
    /* Only complete, synced bytes acquire the public content-addressed name.
     * A concurrent winner is a dedup hit only after verifying its full bytes. */
    if (jash_osif_renameat2(AT_FDCWD, temporary, AT_FDCWD, final, 1u)) {
        int saved = errno;
        srv_unlink_if_identity(temporary, &output_st);
        if (saved != EEXIST || srv_cas_verify(final, size, hash)) return -1;
    } else if (!srv_regular_identity_ok(final, &output_after)) return -1;
    return srv_fsync_dir(dir);
}

static int srv_cas_publish(const char* source, off_t size, const char* hash, char* ref, size_t cap) {
    char path[1100]; struct stat st;
    int n = snprintf(path, sizeof(path), "%s/.jash/overflow/blake3-%s-%lld.bin", srv_home(), hash, (long long)size);
    if (n < 0 || (size_t)n >= sizeof(path) || size < 0) return -1;
    uint64_t additional = lstat(path, &st) == 0 ? 0 : (uint64_t)size;
    int admission;
    if (srv_store_admit(additional, path, &admission)) return -1;
    int rc = srv_cas_publish_impl(source, size, hash, ref, cap);
    if (admission >= 0) close(admission);
    return rc;
}

static void srv_hex(const uint8_t* in, size_t n, char* out) {
    static const char h[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 2] = h[in[i] >> 4];
        out[i * 2 + 1] = h[in[i] & 15];
    }
    out[n * 2] = '\0';
}

/* POSIX paths are byte strings, not guaranteed UTF-8. Encode every high byte
 * as \u00XX so the JSON remains valid and byte-preserving; cwd_bytes_hex is
 * emitted alongside it as the unambiguous machine representation. */
static void srv_json_bytes_esc(const char* s, char* out, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; s && s[i] && o + 7 < cap; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
            case '"':  out[o++] = '\\'; out[o++] = '"';  break;
            case '\\': out[o++] = '\\'; out[o++] = '\\'; break;
            case '\n': out[o++] = '\\'; out[o++] = 'n';  break;
            case '\r': out[o++] = '\\'; out[o++] = 'r';  break;
            case '\t': out[o++] = '\\'; out[o++] = 't';  break;
            default:
                if (c < 0x20 || c >= 0x80)
                    o += (size_t)snprintf(out + o, cap - o, "\\u%04x", c);
                else out[o++] = (char)c;
        }
    }
    out[o] = '\0';
}

static void srv_b64(const uint8_t* in, size_t n, char* out) {
    static const char b[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i = 0, o = 0;
    while (i + 2 < n) {
        uint32_t v = ((uint32_t)in[i] << 16) |
                     ((uint32_t)in[i + 1] << 8) | in[i + 2];
        out[o++] = b[(v >> 18) & 63]; out[o++] = b[(v >> 12) & 63];
        out[o++] = b[(v >> 6) & 63]; out[o++] = b[v & 63];
        i += 3;
    }
    if (i < n) {
        uint32_t v = (uint32_t)in[i] << 16;
        out[o++] = b[(v >> 18) & 63];
        if (i + 1 < n) {
            v |= (uint32_t)in[i + 1] << 8;
            out[o++] = b[(v >> 12) & 63]; out[o++] = b[(v >> 6) & 63];
        } else {
            out[o++] = b[(v >> 12) & 63]; out[o++] = '=';
        }
        out[o++] = '=';
    }
    out[o] = '\0';
}

#ifdef JASH_AGENT_ONLY
#define SRV_PUMP_CHUNK 4096

static int srv_pump_chunk(int respfd, int capture_fd, const char* id,
                          const char* stream, unsigned* sequence,
                          const uint8_t* data, size_t len) {
    if (srv_write_all(capture_fd, (const char*)data, len) != 0) return -1;
    char encoded[4 * ((SRV_PUMP_CHUNK + 2) / 3) + 1];
    char event[sizeof encoded + 256];
    srv_b64(data, len, encoded);
    int n = snprintf(event, sizeof event,
        "{\"id\":%s,\"event\":\"chunk\",\"protocol_version\":2,"
        "\"stream\":\"%s\",\"seq\":%u,\"data_b64\":\"%s\"}\n",
        id, stream, (*sequence)++, encoded);
    if (n <= 0 || (size_t)n >= sizeof event) return -1;
    return srv_write_all(respfd, event, (size_t)n);
}

/* The evaluator remains in the daemon so sticky shell state survives. This
 * child only pumps three pipes. If the JSONL consumer stops reading, its
 * blocked response fd stops this pump, the output pipes fill, and the command
 * receives real kernel backpressure instead of writing an unbounded spool. */
static int srv_pump_io(int respfd, int stdout_fd, int stderr_fd, int stdin_fd,
                       int stdout_capture, int stderr_capture, const char* id,
                       const uint8_t* input, size_t input_len) {
    (void)signal(SIGPIPE, SIG_IGN);
    struct pollfd fds[3] = {
        {stdout_fd, POLLIN, 0},
        {stderr_fd, POLLIN, 0},
        {stdin_fd, input_len ? POLLOUT : 0, 0},
    };
    if (!input_len) {
        close(stdin_fd);
        fds[2].fd = -1;
    }
    size_t input_off = 0;
    unsigned sequence = 0;
    uint8_t buf[SRV_PUMP_CHUNK];
    while (fds[0].fd >= 0 || fds[1].fd >= 0 || fds[2].fd >= 0) {
        int ready = poll(fds, 3, -1);
        if (ready < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        for (int i = 0; i < 2; i++) {
            if (fds[i].fd < 0 ||
                !(fds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            ssize_t n = read(fds[i].fd, buf, sizeof buf);
            if (n > 0) {
                if (srv_pump_chunk(respfd,
                                   i == 0 ? stdout_capture : stderr_capture,
                                   id, i == 0 ? "stdout" : "stderr",
                                   &sequence, buf, (size_t)n) != 0)
                    return -1;
            } else if (n == 0) {
                close(fds[i].fd);
                fds[i].fd = -1;
            } else if (errno != EINTR && errno != EAGAIN) {
                return -1;
            }
        }
        if (fds[2].fd >= 0 &&
            (fds[2].revents & (POLLOUT | POLLHUP | POLLERR))) {
            ssize_t n = write(fds[2].fd, input + input_off,
                              input_len - input_off > SRV_PUMP_CHUNK
                                  ? SRV_PUMP_CHUNK : input_len - input_off);
            if (n > 0) input_off += (size_t)n;
            else if (n < 0 && (errno == EPIPE || errno == ECONNRESET))
                input_off = input_len;
            else if (n < 0 && errno != EINTR && errno != EAGAIN)
                return -1;
            if (input_off == input_len) {
                close(fds[2].fd);
                fds[2].fd = -1;
            }
        }
    }
    return 0;
}
#endif

static int srv_receipt_seed_read(const char* path, uint8_t seed[ED25519_SEED]) {
    int flags = O_RDONLY;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    int fd = open(path, flags);
    if (fd < 0) return -1;
    struct stat st;
    int ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) &&
             (st.st_mode & 077) == 0 && st.st_size == ED25519_SEED &&
             read(fd, seed, ED25519_SEED) == ED25519_SEED;
    close(fd);
    return ok ? 0 : -1;
}

static int srv_receipt_signer_load(srv_receipt_signer_t* signer) {
    memset(signer, 0, sizeof *signer);
    char dir[896];
    if (srv_private_dir("keys", dir, sizeof dir) != 0) return -1;
    char path[1024];
    int pn = snprintf(path, sizeof path, "%s/ed25519.seed", dir);
    if (pn <= 0 || (size_t)pn >= sizeof path) return -1;

    uint8_t seed[ED25519_SEED];
    if (srv_receipt_seed_read(path, seed) != 0) {
        if (errno != ENOENT && access(path, F_OK) == 0) return -1;
        int urandom = open("/dev/urandom", O_RDONLY);
        if (urandom < 0 || read(urandom, seed, sizeof seed) != (ssize_t)sizeof seed) {
            if (urandom >= 0) close(urandom);
            return -1;
        }
        close(urandom);
        char tmp[1088];
        int tn = snprintf(tmp, sizeof tmp, "%s.tmp.%d", path, (int)getpid());
        if (tn <= 0 || (size_t)tn >= sizeof tmp) {
            memset(seed, 0, sizeof seed);
            return -1;
        }
        int flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef O_NOFOLLOW
        flags |= O_NOFOLLOW;
#endif
        int fd = open(tmp, flags, 0600);
        if (fd < 0) { memset(seed, 0, sizeof seed); return -1; }
        int ok = srv_write_all(fd, (const char*)seed, sizeof seed) == 0 &&
                 fsync(fd) == 0;
        close(fd);
        if (!ok) {
            unlink(tmp);
            memset(seed, 0, sizeof seed);
            return -1;
        }
        if (link(tmp, path) != 0) {
            int link_errno = errno;
            unlink(tmp);
            if (link_errno != EEXIST) {
                memset(seed, 0, sizeof seed);
                return -1;
            }
            memset(seed, 0, sizeof seed);
            if (srv_receipt_seed_read(path, seed) != 0 ||
                srv_fsync_dir(dir) != 0) return -1;
            goto seed_ready;
        }
        unlink(tmp);
        if (srv_fsync_dir(dir) != 0) {
            memset(seed, 0, sizeof seed);
            return -1;
        }
    } else if (srv_fsync_dir(dir) != 0) {
        memset(seed, 0, sizeof seed);
        return -1;
    }
seed_ready:
    ed25519_keypair_from_seed(seed, signer->pk, signer->sk);
    memset(seed, 0, sizeof seed);
    srv_hex(signer->pk, sizeof signer->pk, signer->pubkey_hex);
    signer->ready = 1;
    return 0;
}

static int srv_receipt_publish(const srv_receipt_signer_t* signer,
                               const srv_req_t* req, const srv_idem_t* idem,
                               const char* base_response,
                               const char* stdout_hash, const char* stderr_hash,
                               const char* effective_cwd,
                               int rc, double ms, int timed_out, int cancelled,
                               char* ref, size_t refcap,
                               char receipt_hash[JASH_BLAKE3_HEX_LEN]) {
    if (!signer || !signer->ready) return -1;
    char result_hash[JASH_BLAKE3_HEX_LEN], caps_hash[JASH_BLAKE3_HEX_LEN];
    uint8_t digest[32];
    jash_blake3(base_response, strlen(base_response), digest);
    jash_blake3_hex(digest, result_hash);
    struct blake3 caps;
    blake3_init(&caps);
    for (int i = 0; i < req->caps_n; i++) {
        blake3_update(&caps, req->caps[i], strlen(req->caps[i]));
        const char zero = '\0';
        blake3_update(&caps, &zero, 1);
    }
    blake3_final(&caps, digest, sizeof digest);
    jash_blake3_hex(digest, caps_hash);

    char eid[128], cwd_hex[JASH_CWD_MAX * 2 + 1];
    jash_json_esc(req->id_value, eid, sizeof eid);
    srv_hex((const uint8_t*)effective_cwd, strlen(effective_cwd), cwd_hex);
    char payload[2048];
    int plen = snprintf(payload, sizeof payload,
        "{\"schema\":\"jash.agent-runtime-receipt.v2\","
        "\"runtime\":\"%s\",\"version\":\"%s\",\"request_id\":\"%s\","
        "\"request_blake3\":\"%s\",\"result_blake3\":\"%s\","
        "\"stdout_blake3\":\"%s\",\"stderr_blake3\":\"%s\","
        "\"caps_blake3\":\"%s\",\"cwd_bytes_hex\":\"%s\",\"rc\":%d,"
        "\"duration_ms\":%.0f,\"timed_out\":%s,\"cancelled\":%s}",
        JASH_AGENT_RUNTIME_NAME, JASH_VERSION, eid, idem->digest, result_hash,
        stdout_hash, stderr_hash, caps_hash, cwd_hex, rc, ms,
        timed_out ? "true" : "false", cancelled ? "true" : "false");
    if (plen <= 0 || (size_t)plen >= sizeof payload) return -1;

    static const char type[] =
        "application/vnd.jash.agent-runtime-receipt.v2+json";
    uint8_t pae[ED25519_MAX_MSG];
    int paelen = snprintf((char*)pae, sizeof pae, "DSSEv1 %zu %s %d ",
                          strlen(type), type, plen);
    if (paelen <= 0 || (size_t)paelen + (size_t)plen > sizeof pae) return -1;
    memcpy(pae + paelen, payload, (size_t)plen);
    paelen += plen;
    uint8_t sig[ED25519_SIG];
    if (ed25519_sign(sig, pae, (uint32_t)paelen, signer->sk) != 0) return -1;

    char payload_b64[4 * ((sizeof payload + 2) / 3) + 1];
    char sig_b64[4 * ((ED25519_SIG + 2) / 3) + 1];
    srv_b64((const uint8_t*)payload, (size_t)plen, payload_b64);
    srv_b64(sig, sizeof sig, sig_b64);
    char envelope[4096];
    int en = snprintf(envelope, sizeof envelope,
        "{\"payloadType\":\"%s\",\"payload\":\"%s\","
        "\"signatures\":[{\"keyid\":\"%s\",\"sig\":\"%s\"}]}\n",
        type, payload_b64, signer->pubkey_hex, sig_b64);
    if (en <= 0 || (size_t)en >= sizeof envelope) return -1;
    jash_blake3(envelope, (size_t)en, digest);
    jash_blake3_hex(digest, receipt_hash);

    char dir[896];
    if (srv_private_dir("agent_receipts", dir, sizeof dir) != 0) return -1;
    int rn = snprintf(ref, refcap, "%s.dsse", idem->digest);
    if (rn <= 0 || (size_t)rn >= refcap) return -1;
    char final[1100], tmp[1150];
    if (snprintf(final, sizeof final, "%s/%s", dir, ref) <= 0 ||
        snprintf(tmp, sizeof tmp, "%s.tmp.%d", final, (int)getpid()) <= 0)
        return -1;
    int flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    int fd = open(tmp, flags, 0600);
    if (fd < 0) return -1;
    int ok = srv_write_all(fd, envelope, (size_t)en) == 0 && fsync(fd) == 0;
    close(fd);
    if (!ok || rename(tmp, final) != 0 || srv_fsync_dir(dir) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

static int srv_typed_receipt_publish(
        const srv_receipt_signer_t* signer, const char* payload,
        const char* ref, char receipt_hash[JASH_BLAKE3_HEX_LEN], const char* directory, const char* type) {
    if (!signer || !signer->ready || !payload || !ref || !*ref ||
        strchr(ref, '/') || strstr(ref, "..")) return -1;
    size_t payload_len = strlen(payload);
    uint8_t pae[ED25519_MAX_MSG];
    int paelen = snprintf((char*)pae, sizeof pae, "DSSEv1 %zu %s %zu ",
                          strlen(type), type, payload_len);
    if (paelen <= 0 || (size_t)paelen + payload_len > sizeof pae) return -1;
    memcpy(pae + paelen, payload, payload_len);
    paelen += (int)payload_len;
    uint8_t signature[ED25519_SIG];
    if (ed25519_sign(signature, pae, (uint32_t)paelen, signer->sk) != 0)
        return -1;

    char payload_b64[4 * ((4096 + 2) / 3) + 1];
    char signature_b64[4 * ((ED25519_SIG + 2) / 3) + 1];
    if (payload_len > 4096) return -1;
    srv_b64((const uint8_t*)payload, payload_len, payload_b64);
    srv_b64(signature, sizeof signature, signature_b64);
    char envelope[8192];
    int envelope_len = snprintf(envelope, sizeof envelope,
        "{\"payloadType\":\"%s\",\"payload\":\"%s\","
        "\"signatures\":[{\"keyid\":\"%s\",\"sig\":\"%s\"}]}\n",
        type, payload_b64, signer->pubkey_hex, signature_b64);
    if (envelope_len <= 0 || (size_t)envelope_len >= sizeof envelope)
        return -1;

    uint8_t digest[32];
    jash_blake3(envelope, (size_t)envelope_len, digest);
    jash_blake3_hex(digest, receipt_hash);

    char dir[896], final[1100];
    int final_len = srv_private_dir(directory, dir, sizeof dir) == 0
        ? snprintf(final, sizeof final, "%s/%s", dir, ref) : -1;
    if (final_len <= 0 || (size_t)final_len >= sizeof final)
        return -1;
    int flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    int fd = open(final, flags, 0600);
    if (fd < 0) {
        if (errno != EEXIST ||
            srv_cas_verify(final, (off_t)envelope_len, receipt_hash) != 0)
            return -1;
        return srv_fsync_dir(dir);
    }
    struct stat opened = {0}, after;
    int ok = fstat(fd, &opened) == 0 && S_ISREG(opened.st_mode) &&
             opened.st_uid == geteuid() && opened.st_nlink == 1 &&
             (opened.st_mode & 0777) == 0600 &&
             srv_write_all(fd, envelope, (size_t)envelope_len) == 0 &&
             fsync(fd) == 0 && fstat(fd, &after) == 0 &&
             after.st_dev == opened.st_dev && after.st_ino == opened.st_ino &&
             after.st_nlink == 1 && srv_regular_identity_ok(final, &after);
    close(fd);
    if (!ok) {
        srv_unlink_if_identity(final, &opened);
        (void)srv_fsync_dir(dir);
        return -1;
    }
    return srv_fsync_dir(dir);
}

static int srv_job_receipt_publish(const srv_receipt_signer_t* signer, const char* payload,
                                    const char* ref, char hash[JASH_BLAKE3_HEX_LEN]) {
    return srv_typed_receipt_publish(signer, payload, ref, hash, "agent_receipts",
        "application/vnd.jash.agent-job-receipt.v1+json");
}

static int srv_idem_begin_id(const char* id_value, const char* request,
                             srv_idem_t* idem, char** replay) {
    *replay = NULL;
    idem->fd = -1;
    uint8_t digest[32], id_digest[32];
    char id_hex[JASH_BLAKE3_HEX_LEN];
    jash_blake3(request, strlen(request), digest);
    jash_blake3_hex(digest, idem->digest);
    jash_blake3(id_value, strlen(id_value), id_digest);
    jash_blake3_hex(id_digest, id_hex);

    char dir[896];
    if (srv_private_dir("agent_requests", dir, sizeof(dir)) != 0)
        return SRV_IDEM_ERROR;
    int pn = snprintf(idem->path, sizeof(idem->path), "%s/%s.req", dir, id_hex);
    if (pn <= 0 || (size_t)pn >= sizeof(idem->path)) return SRV_IDEM_ERROR;

    int flags = O_RDWR | O_APPEND | O_CREAT | O_EXCL;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    int fd = srv_cloexec(open(idem->path, flags, 0600));
    if (fd >= 0) {
        char state[96];
        int n = snprintf(state, sizeof(state), "%s\nrunning\n", idem->digest);
        int ok = n > 0 && (size_t)n < sizeof(state) &&
                 srv_write_all(fd, state, (size_t)n) == 0 && fsync(fd) == 0;
        if (!ok || srv_fsync_dir(dir) != 0) {
            close(fd);
            return SRV_IDEM_ERROR;
        }
        idem->fd = fd;
        return SRV_IDEM_NEW;
    }
    if (errno != EEXIST) return SRV_IDEM_ERROR;

    flags = O_RDONLY;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    fd = open(idem->path, flags);
    if (fd < 0) return SRV_IDEM_ERROR;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_uid != geteuid() || (st.st_mode & 0777) != 0600 ||
        st.st_nlink != 1 || !srv_regular_identity_ok(idem->path, &st) ||
        st.st_size < 66 || st.st_size > SRV_IDEM_MAX) {
        close(fd);
        return SRV_IDEM_INDETERMINATE;
    }
    size_t size = (size_t)st.st_size;
    char* data = (char*)malloc(size + 1);
    if (!data) { close(fd); return SRV_IDEM_ERROR; }
    size_t off = 0;
    while (off < size) {
        ssize_t n = read(fd, data + off, size - off);
        if (n > 0) { off += (size_t)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        break;
    }
    close(fd);
    data[off] = '\0';
    if (off != size) { free(data); return SRV_IDEM_INDETERMINATE; }
    char* first = strchr(data, '\n');
    if (!first) { free(data); return SRV_IDEM_INDETERMINATE; }
    *first = '\0';
    if (strcmp(data, idem->digest) != 0) {
        free(data);
        return SRV_IDEM_CONFLICT;
    }
    char* state = first + 1;
    char* second = strchr(state, '\n');
    if (!second) { free(data); return SRV_IDEM_INDETERMINATE; }
    *second = '\0';
    if (strcmp(state, "completed") == 0 && second[1]) {
        *replay = strdup(second + 1);
        free(data);
        return *replay ? SRV_IDEM_REPLAY : SRV_IDEM_ERROR;
    }
    if (strcmp(state, "running") == 0 && second[1]) {
        int status = srv_idem_appended_replay(second + 1, data + size, replay);
        free(data);
        return status;
    }
    free(data);
    return SRV_IDEM_INDETERMINATE;
}

static int srv_idem_begin(const srv_req_t* r, const char* request,
                          srv_idem_t* idem, char** replay) {
    return srv_idem_begin_id(r->id_value, request, idem, replay);
}

static int srv_idem_complete(srv_idem_t* idem, const char* response) {
    if (!idem || idem->fd < 0 || !response) return -1;
    size_t response_len = strlen(response);
    if (response_len == 0 || response_len > SRV_RESPONSE_MAX) return -1;

    char running[96];
    int running_len = snprintf(running, sizeof running, "%s\nrunning\n",
                               idem->digest);
    struct stat opened, named;
    char observed[96];
    if (running_len <= 0 || (size_t)running_len >= sizeof running ||
        fstat(idem->fd, &opened) != 0 || !S_ISREG(opened.st_mode) ||
        opened.st_uid != geteuid() || (opened.st_mode & 0777) != 0600 ||
        opened.st_nlink != 1 ||
        opened.st_size != running_len ||
        lstat(idem->path, &named) != 0 || !S_ISREG(named.st_mode) ||
        S_ISLNK(named.st_mode) || opened.st_dev != named.st_dev ||
        opened.st_ino != named.st_ino || named.st_nlink != 1 ||
        pread(idem->fd, observed, (size_t)running_len, 0) != running_len ||
        memcmp(observed, running, (size_t)running_len) != 0)
        return -1;

    uint8_t digest[32];
    char response_hash[JASH_BLAKE3_HEX_LEN];
    jash_blake3(response, response_len, digest);
    jash_blake3_hex(digest, response_hash);
    char frame[128];
    int frame_len = snprintf(frame, sizeof frame, "completed-v1\n%zu\n%s\n",
                             response_len, response_hash);
    if (frame_len <= 0 || (size_t)frame_len >= sizeof frame) return -1;

    if (srv_write_all(idem->fd, frame, (size_t)frame_len) != 0 ||
        srv_write_all(idem->fd, response, response_len) != 0 ||
        fsync(idem->fd) != 0)
        return -1;

    if (lstat(idem->path, &named) != 0 || !S_ISREG(named.st_mode) ||
        opened.st_dev != named.st_dev || opened.st_ino != named.st_ino)
        return -1;
    return 0;
}

static void srv_idem_finish(int respfd, srv_idem_t* idem,
                            const char* id, const char* response) {
    if (srv_idem_complete(idem, response) == 0) {
        (void)srv_write_all(respfd, response, strlen(response));
        return;
    }
    char fallback[256];
    int n = snprintf(fallback, sizeof(fallback),
                     "{\"id\":%s,\"error\":\"idempotency commit failed\","
                     "\"outcome\":\"indeterminate\"}\n", id);
    if (n > 0 && (size_t)n < sizeof(fallback))
        (void)srv_write_all(respfd, fallback, (size_t)n);
}

static int srv_idem_abort(srv_idem_t* idem) {
    if (!idem) return -1;
    char dir[1024];
    snprintf(dir, sizeof dir, "%s", idem->path);
    char* slash = strrchr(dir, '/');
    if (!slash) return -1;
    *slash = '\0';
    struct stat opened;
    int pinned = idem->fd >= 0 && fstat(idem->fd, &opened) == 0 &&
                 srv_regular_identity_ok(idem->path, &opened);
    if (idem->fd >= 0) {
        close(idem->fd);
        idem->fd = -1;
    }
    if (!pinned) return -1;
    srv_unlink_if_identity(idem->path, &opened);
    if (lstat(idem->path, &opened) == 0 || errno != ENOENT) return -1;
    return srv_fsync_dir(dir);
}

/* Rejections discovered after reserving the id but before evaluation have no
 * effect to make exactly-once. Remove the running reservation durably instead
 * of storing an unsigned completed response. */
static void srv_idem_reject(int respfd, srv_idem_t* idem,
                            const char* id, const char* response) {
    if (srv_idem_abort(idem) != 0) {
        srv_dprintf(respfd, "{\"id\":%s,\"error\":\"idempotency abort failed\","
                "\"outcome\":\"indeterminate\"}\n", id);
        return;
    }
    (void)srv_write_all(respfd, response, strlen(response));
}

static int srv_has_cap(const srv_req_t* r, const char* cap) {
    for (int i = 0; i < r->caps_n; i++)
        if (strcmp(r->caps[i], cap) == 0) return 1;
    return 0;
}

static int srv_cancel_id_safe(const char* id) {
    for (const unsigned char* p = (const unsigned char*)id; *p; p++)
        if (!(isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == ':'))
            return 0;
    return 1;
}

static void srv_cancel_discard_stale(jash_state_t* s) {
    if (!s || s->agent_cancel_fd < 0) return;
    s->agent_cancel_len = 0;
    s->agent_cancel_discarding = 0;
    s->agent_cancel_buf[0] = '\0';
    char discard[512];
    while (read(s->agent_cancel_fd, discard, sizeof discard) > 0) {}
}

static const char* srv_check_caps(const jash_state_t* s, const srv_req_t* r) {
    if (!r->caps_seen || r->caps_n == 0) return "capabilities required";
    for (int i = 0; i < r->caps_n; i++) {
        if (strcmp(r->caps[i], "effect:read") != 0 &&
            strcmp(r->caps[i], "effect:mutate") != 0 &&
            strcmp(r->caps[i], "effect:exec") != 0)
            return "unknown capability";
    }
    if (r->line) return "text line has no provable effect";
    const char* effect = jash_builtin_effect_argv(r->argv[0], r->argc - 1, r->argv + 1);
    if (strcmp(effect, "unknown") == 0) {
        if (!srv_has_cap(r, "effect:exec")) return "command effect unknown";
        effect = "exec";
    }
    if (r->timeout_ms >= 0 && strcmp(effect, "exec") != 0)
        return "timeout_ms requires effect:exec command";
    int native_cancel = jash_mutation_cancelable(r->argv[0], r->argc - 1, r->argv + 1) ||
                        jash_query_cancelable(r->argv[0], r->argc - 1, r->argv + 1);
    if (r->cancelable && !native_cancel && (strcmp(effect, "exec") != 0 || r->timeout_ms < 0))
        return "cancelable requires effect:exec command with timeout_ms";
    if (r->cancelable && s->agent_cancel_fd < 0)
        return "cancelable requires JASH_AGENT_CANCEL_FD";
    if (r->cancelable && !srv_cancel_id_safe(r->id_value))
        return "cancelable id must use [A-Za-z0-9._:-]";
    char required[32];
    snprintf(required, sizeof(required), "effect:%s", effect);
    return srv_has_cap(r, required) ? NULL : "capability denied";
}

static int srv_native_cancel(void* context) {
    jash_state_t* state = context;
    if (!srv_shutdown_requested() && !jash_agent_cancel_poll(state)) return 0;
    state->agent_request_cancelled = 1;
    return 1;
}
#endif

/* ── argv → shell-safe line (single-quote per token; '\'' for quotes) ───── */

/* JINV-016 — argv[0] tem que ser citado como QUALQUER outro token.
 *
 * A tese declarada deste protocolo (serve.h, FEATURES.md §5.2) é "argv é
 * pré-tokenizado — a classe BUG-059 não pode existir por construção". Mas
 * argv[0] era emitido BARE, então o token 0 voltava a ser TEXTO DE SHELL:
 *
 *     {"argv":["echo hi; touch /tmp/pwned"]}   →  echo hi; touch /tmp/pwned
 *
 * Dois comandos. Comprovado ao vivo nas duas portas (`--exec-json` e
 * `--serve`) antes desta correção. Um agente que monta argv a partir de dados
 * não-confiáveis — que é exatamente o caso de uso — entregava execução
 * arbitrária pelo campo que a documentação dizia ser o mais seguro.
 *
 * Citar o token 0 é seguro: `'cd' /tmp`, `'echo' x`, `'pwd'` continuam
 * despachando builtins normalmente (a citação só desliga alias/keyword, que é
 * a semântica CERTA para "isto é um nome de comando literal").
 * Gate: tests/test_jinv016_execjson_inject.sh
 */
static int srv_argv_to_line(const srv_req_t* r, char* out, size_t cap) {
    size_t o = 0;
    for (int i = 0; i < r->argc; i++) {
        const char* a = r->argv[i];
        if (i) { if (o + 1 >= cap) return -1; out[o++] = ' '; }
        if (o + 1 >= cap) return -1;
        out[o++] = '\'';
        for (const char* p = a; *p; p++) {
            if (*p == '\'') {
                if (o + 4 >= cap) return -1;
                out[o++]='\''; out[o++]='\\'; out[o++]='\''; out[o++]='\'';
            } else {
                if (o + 1 >= cap) return -1;
                out[o++] = *p;
            }
        }
        if (o + 1 >= cap) return -1;
        out[o++] = '\'';
    }
    out[o] = '\0';
    return 0;
}

/* ── JINV-017: a porta ÚNICA do pedido estruturado ──────────────────────── */

/* `--exec-json` e `--serve` são o MESMO protocolo. Antes disto havia dois
 * parsers: este (completo: argv/line/cwd/env/stdin, com jp_skip para chaves
 * desconhecidas) e um `exec_json_to_cmd` em main.c feito de `strstr` que só
 * entendia `argv` — e ACEITAVA `cwd`, `env` e `stdin` calados, sem aplicar
 * nada. Comprovado ao vivo antes desta correção:
 *
 *     --exec-json '{"argv":["pwd"],"cwd":"/tmp"}'          → imprimia $PWD
 *     --exec-json '{"argv":["sh","-c","echo $A"],"env":…}' → imprimia vazio
 *     --exec-json '{"argv":["cat"],"stdin":"hello"}'       → lia /dev/null
 *
 * Aceitar um campo e ignorá-lo é a falha mais cara que um protocolo pode ter:
 * o chamador acredita que pediu isolamento (`cwd` de scratch) e recebe
 * execução no diretório errado, sem um único aviso. Por isso esta porta é
 * FAIL-CLOSED — se `cwd` não existe, ela RECUSA em vez de rodar em outro
 * lugar; se `stdin` não pôde ser entregue, ela RECUSA em vez de ler o
 * terminal.
 *
 * `exec_json_to_cmd` continua existindo em main.c, agora encaminhando para
 * cá: nenhuma declaração foi removida, ela passou a ter uma implementação só.
 * Gate: tests/test_jinv017_execjson_context.sh
 */
const char* jash_exec_json(const char* json, char* out, size_t cap,
                           int apply_context) {
    if (!json || !out || cap == 0) return "argumentos internos inválidos";

    srv_req_t* r = (srv_req_t*)malloc(sizeof *r);
    if (!r) return "sem memória";

    const char* err = srv_parse(json, r);

    /* 1. renderiza o comando (argv citado token a token, ou `line` literal) */
    if (!err) {
        if (r->line) {
            size_t n = strlen(r->line);
            if (n + 1 > cap) err = "comando longo demais";
            else memcpy(out, r->line, n + 1);
        } else if (srv_argv_to_line(r, out, cap) != 0) {
            err = "comando longo demais";
        }
    }

    /* 2. aplica o CONTEXTO ao processo. Ordem: env → cwd → stdin, porque uma
     *    variável de ambiente pode nomear o diretório. */
    if (!err && apply_context) {
        for (int i = 0; i < r->env_n && !err; i++)
            if (setenv(r->env_k[i], r->env_v[i], 1) != 0)
                err = "env: setenv falhou";

        if (!err && r->cwd[0] && chdir(r->cwd) != 0)
            err = "cwd: diretório inacessível — recusando rodar em outro lugar";

        if (!err && r->stdin_buf) {
            /* Arquivo temporário ANÔNIMO (mkstemp+unlink, idioma do JINV-010):
             * um pipe travaria sozinho acima do buffer do kernel, já que não há
             * segundo processo para drenar enquanto escrevemos. */
            const char* td = getenv("TMPDIR");
            if (!td || !*td) td = jash_tmpdir();
            char tpl[512];
            snprintf(tpl, sizeof tpl, "%s/jash-execjson-stdin-XXXXXX", td);
            int fd = mkstemp(tpl);
            if (fd < 0) {
                err = "stdin: não consegui criar o temporário";
            } else {
                unlink(tpl);                     /* some do namespace na hora */
                size_t off = 0;
                while (off < r->stdin_len) {
                    ssize_t w = write(fd, r->stdin_buf + off,
                                      r->stdin_len - off);
                    if (w <= 0) { err = "stdin: escrita curta"; break; }
                    off += (size_t)w;
                }
                if (!err && lseek(fd, 0, SEEK_SET) != 0)
                    err = "stdin: rebobinar falhou";
                if (!err && dup2(fd, 0) < 0)
                    err = "stdin: dup2 falhou";
                close(fd);
            }
        }
    }

    srv_req_free(r);
    free(r);
    return err;
}

/* ── capture plumbing ───────────────────────────────────────────────────── */

static long srv_fd1_writer(void* ctx, const char* buf, size_t n) {
    (void)ctx;
    ssize_t wr;
    do { wr = write(1, buf, n); } while (wr < 0 && errno == EINTR);
    return (long)wr;
}

/* Read a capture file; inline small streams, move big ones to overflow.
 * Emits the JSON fields for one stream into out. */
static int srv_stream_json(const char* tag, const char* path, int seq,
                            char* out, size_t cap,
                            char hash[JASH_BLAKE3_HEX_LEN]) {
    size_t offset = strlen(out);
    if (offset >= cap) return -1;
    if (hash && srv_hash_file(path, hash) != 0) return -1;
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size < 0) return -1;
    jash_json_buffer_t buffer = {out, cap, offset};
    jash_json_writer_t writer;
    jash_jsonw_init(&writer, jash_jsonw_buffer_sink, &buffer);
    if (st.st_size <= SRV_INLINE_MAX) {
        size_t size = (size_t)st.st_size;
        char* raw = malloc(size ? size : 1);
        if (!raw) return -1;
        int fd = open(path, O_RDONLY);
        if (fd < 0) { free(raw); return -1; }
        size_t used = 0;
        while (used < size) {
            ssize_t n = read(fd, raw + used, size - used);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) { close(fd); free(raw); return -1; }
            used += (size_t)n;
        }
        close(fd);
        jash_jsonw_raw(&writer, ",");
        jash_jsonw_string(&writer, tag);
        jash_jsonw_raw(&writer, ":");
        jash_jsonw_text_or_bytes(&writer, raw, size);
        jash_jsonw_raw(&writer, ",");
        jash_jsonw_string_pair(&writer, tag, "_bytes");
        jash_jsonw_raw(&writer, ":");
        jash_jsonw_safe_u64(&writer, size);
        free(raw);
        return jash_jsonw_finish(&writer);
    }
    /* A large capture must have a retained reference, never just a preview. */
    const char* home = getenv("HOME");
    if (!home || !*home) return -1;
    char dir[512], destination[768];
    int n = snprintf(dir, sizeof dir, "%s/.jash/overflow", home);
    if (n < 0 || (size_t)n >= sizeof dir) return -1;
    if (mkdir(dir, 0700) && errno != EEXIST) return -1;
    n = snprintf(destination, sizeof destination, "%s/%d-serve%d-%s.txt",
                 dir, (int)getpid(), seq, tag);
    if (n < 0 || (size_t)n >= sizeof destination ||
        rename(path, destination)) return -1;
    const char* ref = strrchr(destination, '/');
    ref = ref ? ref + 1 : destination;
    unsigned char head[1024];
    int fd = open(destination, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t got;
    do { got = read(fd, head, sizeof head); } while (got < 0 && errno == EINTR);
    close(fd);
    if (got < 0) return -1;
    jash_jsonw_raw(&writer, ",");
    jash_jsonw_string_pair(&writer, tag, "_ref");
    jash_jsonw_raw(&writer, ":"); jash_jsonw_string(&writer, ref);
    jash_jsonw_raw(&writer, ",");
    jash_jsonw_string_pair(&writer, tag, "_bytes");
    jash_jsonw_raw(&writer, ":"); jash_jsonw_safe_u64(&writer, (uint64_t)st.st_size);
    jash_jsonw_raw(&writer, ",");
    jash_jsonw_string_pair(&writer, tag, "_head");
    jash_jsonw_raw(&writer, ":");
    jash_jsonw_text_or_bytes(&writer, head, (size_t)got);
    return jash_jsonw_finish(&writer);
}

#ifdef JASH_AGENT_ONLY
static int srv_stream_v2_json(const char* tag, const char* path,
                              char* out, size_t cap,
                              char hash[JASH_BLAKE3_HEX_LEN]) {
    struct stat st;
    int fd = srv_open_regular_nofollow(path, &st);
    if (fd < 0 || st.st_size < 0 ||
        srv_hash_regular_fd(fd, st.st_size, hash) != 0) {
        if (fd >= 0) close(fd);
        return -1;
    }
    close(fd);
    char ref[160];
    if (srv_cas_publish(path, st.st_size, hash, ref, sizeof ref) != 0)
        return -1;
    size_t o = strlen(out);
    int n = snprintf(out + o, cap - o,
                     ",\"%s_ref\":\"%s\",\"%s_bytes\":%lld,"
                     "\"%s_blake3\":\"%s\"",
                     tag, ref, tag, (long long)st.st_size, tag, hash);
    return n > 0 && (size_t)n < cap - o ? 0 : -1;
}
#endif

#ifdef JASH_AGENT_ONLY
static void srv_job_error(int respfd, const srv_job_req_t* request,
                          const char* error) {
    char escaped_error[384], escaped_op[64];
    jash_json_esc(error, escaped_error, sizeof escaped_error);
    jash_json_esc(request->op, escaped_op, sizeof escaped_op);
    srv_dprintf(respfd,
            "{\"id\":%s,\"protocol_version\":2,\"op\":\"%s\","
            "\"error\":\"%s\"}\n",
            request->id, escaped_op, escaped_error);
}

static srv_job_t* srv_job_find(srv_job_table_t* table, const char* job_id) {
    for (int i = 0; i < SRV_JOB_MAX; i++)
        if (table->jobs[i].used &&
            strcmp(table->jobs[i].job_id, job_id) == 0)
            return &table->jobs[i];
    return NULL;
}

static srv_job_t* srv_job_find_request(srv_job_table_t* table,
                                       const char* request_id) {
    for (int i = 0; i < SRV_JOB_MAX; i++)
        if (table->jobs[i].used &&
            strcmp(table->jobs[i].request_id, request_id) == 0)
            return &table->jobs[i];
    return NULL;
}

static srv_job_t* srv_job_free_slot(srv_job_table_t* table) {
    for (int i = 0; i < SRV_JOB_MAX; i++)
        if (!table->jobs[i].used) return &table->jobs[i];
    return NULL;
}

static int srv_job_random_id(srv_job_table_t* table,
                             char out[SRV_JOB_ID_MAX]) {
    for (int attempt = 0; attempt < 8; attempt++) {
        uint8_t random[16];
        int flags = O_RDONLY;
#ifdef O_CLOEXEC
        flags |= O_CLOEXEC;
#endif
        int fd = open("/dev/urandom", flags);
        if (fd < 0) return -1;
        size_t offset = 0;
        while (offset < sizeof random) {
            ssize_t count = read(fd, random + offset, sizeof random - offset);
            if (count > 0) offset += (size_t)count;
            else if (count < 0 && errno == EINTR) continue;
            else break;
        }
        close(fd);
        if (offset != sizeof random) return -1;
        char hex[sizeof random * 2 + 1];
        srv_hex(random, sizeof random, hex);
        snprintf(out, SRV_JOB_ID_MAX, "j-%s", hex);
        if (!srv_job_find(table, out)) return 0;
    }
    errno = EEXIST;
    return -1;
}

static const char* srv_job_state_name(jash_agent_process_state_t state) {
    switch (state) {
    case JASH_AGENT_PROCESS_NEW: return "new";
    case JASH_AGENT_PROCESS_RUNNING: return "running";
    case JASH_AGENT_PROCESS_TERM_SENT: return "term_sent";
    case JASH_AGENT_PROCESS_KILL_SENT: return "kill_sent";
    case JASH_AGENT_PROCESS_DONE: return "done";
    case JASH_AGENT_PROCESS_INDETERMINATE: return "indeterminate";
    }
    return "unknown";
}

static void srv_job_release_input_pin(srv_job_t* job) {
    if (!job->stdin_artifact_pin) return;
    srv_data_buffer_t* object = srv_data_find(job->stdin_artifact_ref);
    if (object && object->readers) object->readers--;
    job->stdin_artifact_pin = 0;
}

static int srv_job_refresh(srv_job_t* job) {
    if (jash_agent_process_poll(job->process, 0) < 0) return -1;
    const jash_agent_process_result_t* result =
        jash_agent_process_get_result(job->process);
    if (result && result->state == JASH_AGENT_PROCESS_DONE && result->quiescent)
        srv_job_release_input_pin(job);
    return 0;
}

/* These fields are covered by the existing exact response hash in the receipt.
 * Keep the DSSE payload compact; no independent unsigned quota summary. */
static int srv_job_resource_fields(const jash_agent_process_result_t* r, int done,
                                    char* out, size_t cap) {
    if (!r || !cap) return -1;
    out[0] = 0;
    if (!r->resources_enforced) return 0;
    int n = snprintf(out, cap,
        ",\"resource_backend\":\"cgroup-v2\",\"resource_enforced\":true,"
        "\"resources\":{\"memory_max_bytes\":%llu,\"pids_max\":%llu,"
        "\"cpu_quota_us\":%llu,\"cpu_period_us\":%llu,\"wall_timeout_ms\":%llu}",
        (unsigned long long)r->resources.memory_max_bytes, (unsigned long long)r->resources.pids_max,
        (unsigned long long)r->resources.cpu_quota_us, (unsigned long long)r->resources.cpu_period_us,
        (unsigned long long)r->resources.wall_timeout_ms);
    if (n < 0 || (size_t)n >= cap) return -1;
    if (!done) return n;
    size_t used = (size_t)n;
    n = snprintf(out + used, cap - used,
        ",\"resource_timed_out\":%s,\"resource_violation\":%s,\"resource_empty\":%s,"
        "\"descendants_forced\":%s,"
        "\"resource_memory_events\":%llu,\"resource_pids_events\":%llu",
        r->resource_timed_out ? "true" : "false", r->resource_violation ? "true" : "false",
        r->resource_empty ? "true" : "false", r->descendants_forced ? "true" : "false",
        (unsigned long long)r->resource_memory_events,
        (unsigned long long)r->resource_pids_events);
    return n >= 0 && (size_t)n < cap - used ? (int)(used + (size_t)n) : -1;
}

static int srv_job_input_fields(const srv_job_t* job, char* out, size_t cap) {
    if (!job->stdin_artifact && !job->stdin_stream) {
        if (cap) out[0] = '\0';
        return 0;
    }
    int n;
    if (job->stdin_stream) {
        n = snprintf(out, cap,
            ",\"stdin_stream_ref\":\"%s\",\"stdin_stream_offset\":%llu,"
            "\"stdin_stream_final_ref\":\"%s\","
            "\"stdin_stream_final_blake3\":\"%s\"",
            job->stdin_stream_ref,
            (unsigned long long)job->stdin_stream_offset,
            job->stdin_stream_final_ref, job->stdin_stream_final_hash);
    } else {
        n = snprintf(out, cap, ",\"stdin_artifact_ref\":\"%s\"",
                     job->stdin_artifact_ref);
    }
    if (n < 0 || (size_t)n >= cap) return -1;
    size_t used = (size_t)n;
    n = snprintf(out + used, cap - used,
        ",\"stdin_blake3\":\"%s\","
        "\"stdin_bytes\":%llu,\"stdin_durability\":\"%s\","
        "\"stdin_delivery\":\"fd\",\"stdin_serialized_bytes\":0,"
        "\"stdin_materialized_bytes\":%llu,\"stdin_verified_bytes\":%llu",
        job->stdin_artifact_hash,
        (unsigned long long)job->stdin_artifact_bytes,
        job->stdin_stream ? "sealed-snapshot" :
            job->stdin_artifact_durable ? "durable" : "volatile",
        (unsigned long long)job->stdin_materialized_bytes,
        (unsigned long long)job->stdin_verified_bytes);
    return n >= 0 && (size_t)n < cap - used ? (int)(used + (size_t)n) : -1;
}

static int srv_job_format(const srv_job_req_t* request, const srv_job_t* job,
                          const char* extra, char* out, size_t cap) {
    const jash_agent_process_result_t* result =
        jash_agent_process_get_result(job->process);
    if (!result) return -1;
    int done = result->state == JASH_AGENT_PROCESS_DONE;
    int terminal = done && job->terminal_receipt_ready;
    int n = snprintf(out, cap,
        "{\"id\":%s,\"protocol_version\":2,\"op\":\"%s\","
        "\"job_id\":\"%s\",\"state\":\"%s\",\"terminal\":%s,"
        "\"background\":%s,\"pty\":%s,\"cancel_requested\":%s,"
        "\"kill_sent\":%s",
        request->id, request->op, job->job_id,
        srv_job_state_name(result->state), terminal ? "true" : "false",
        job->background ? "true" : "false",
        result->pty ? "true" : "false",
        result->cancel_requested ? "true" : "false",
        result->kill_sent ? "true" : "false");
    if (n <= 0 || (size_t)n >= cap) return -1;
    size_t used = (size_t)n;
    n = srv_job_resource_fields(result, done, out + used, cap - used);
    if (n < 0) return -1;
    used += (size_t)n;
    n = srv_job_input_fields(job, out + used, cap - used);
    if (n < 0) return -1;
    used += (size_t)n;
    if (done) {
        n = snprintf(out + used, cap - used,
                     ",\"rc\":%d,\"signal\":%d,\"quiescent\":%s,"
                     "\"quiescence_forced\":%s,\"output_limit\":%s,"
                     "\"output_limit_bytes\":%llu",
                     result->exit_code, result->term_signal,
                     result->quiescent ? "true" : "false",
                     result->quiescence_forced ? "true" : "false",
                     result->output_limit_hit ? "true" : "false",
                     (unsigned long long)result->output_limit_bytes);
        if (n <= 0 || (size_t)n >= cap - used) return -1;
        used += (size_t)n;
    }
    if (terminal) {
        n = snprintf(out + used, cap - used,
            ",\"stdout_ref\":\"%s\",\"stdout_blake3\":\"%s\","
            "\"stdout_bytes\":%llu,\"stderr_ref\":\"%s\","
            "\"stderr_blake3\":\"%s\",\"stderr_bytes\":%llu,"
            "\"start_request_blake3\":\"%s\","
            "\"start_receipt_ref\":\"%s\","
            "\"start_receipt_blake3\":\"%s\"",
            job->stdout_ref, job->stdout_hash, job->stdout_bytes,
            job->stderr_ref, job->stderr_hash, job->stderr_bytes,
            job->request_digest, job->start_receipt_ref,
            job->start_receipt_hash);
        if (n <= 0 || (size_t)n >= cap - used) return -1;
        used += (size_t)n;
    }
    if (extra && *extra) {
        n = snprintf(out + used, cap - used, ",%s", extra);
        if (n <= 0 || (size_t)n >= cap - used) return -1;
        used += (size_t)n;
    }
    n = snprintf(out + used, cap - used, "}\n");
    return n > 0 && (size_t)n < cap - used ? 0 : -1;
}

static int srv_job_request_digest(const char* text,
                                  char out[JASH_BLAKE3_HEX_LEN]) {
    uint8_t digest[32];
    jash_blake3(text, strlen(text), digest);
    jash_blake3_hex(digest, out);
    return 0;
}

static int srv_job_stream_to_cas(const char* path, char* ref, size_t refcap,
                                 char hash[JASH_BLAKE3_HEX_LEN],
                                 unsigned long long* bytes,
                                 dev_t* cas_dev, ino_t* cas_ino) {
    struct stat st, after;
    int fd = srv_open_regular_nofollow(path, &st);
    if (fd < 0 || st.st_size < 0 ||
        srv_hash_regular_fd(fd, st.st_size, hash) != 0 ||
        fstat(fd, &after) != 0 || after.st_dev != st.st_dev ||
        after.st_ino != st.st_ino || after.st_size != st.st_size ||
        !srv_regular_identity_ok(path, &after)) {
        if (fd >= 0) close(fd);
        return -1;
    }
    close(fd);
    if (srv_cas_publish(path, st.st_size, hash, ref, refcap) != 0)
        return -1;
    char dir[896], cas_path[1100];
    struct stat cas_st;
    int pn = srv_private_dir("overflow", dir, sizeof dir) == 0
        ? snprintf(cas_path, sizeof cas_path, "%s/%s", dir, ref) : -1;
    int cas_fd = pn > 0 && (size_t)pn < sizeof cas_path
        ? srv_open_regular_nofollow(cas_path, &cas_st) : -1;
    if (cas_fd < 0 || cas_st.st_size != st.st_size) {
        if (cas_fd >= 0) close(cas_fd);
        return -1;
    }
    close(cas_fd);
    *bytes = (unsigned long long)st.st_size;
    *cas_dev = cas_st.st_dev;
    *cas_ino = cas_st.st_ino;
    return 0;
}

static int srv_job_publish_start_receipt(
        const srv_receipt_signer_t* signer, const srv_job_req_t* request,
        srv_job_t* job, const char* base_response) {
    uint8_t digest[32];
    char response_hash[JASH_BLAKE3_HEX_LEN], escaped_id[192];
    jash_blake3(base_response, strlen(base_response), digest);
    jash_blake3_hex(digest, response_hash);
    jash_json_esc(request->id_value, escaped_id, sizeof escaped_id);
    char receipt_ref[sizeof job->start_receipt_ref];
    int rn = snprintf(receipt_ref, sizeof receipt_ref,
                      "%s.job-start.dsse", job->request_digest);
    if (rn <= 0 || (size_t)rn >= sizeof job->start_receipt_ref) return -1;
    memcpy(job->start_receipt_ref, receipt_ref, (size_t)rn + 1);
    char payload[2048];
    int pn = snprintf(payload, sizeof payload,
        "{\"schema\":\"jash.agent-job-receipt.v1\",\"phase\":\"start\","
        "\"request_id\":\"%s\",\"request_blake3\":\"%s\","
        "\"response_blake3\":\"%s\",\"job_id\":\"%s\","
        "\"accepted\":true,\"terminal\":false}",
        escaped_id, job->request_digest, response_hash, job->job_id);
    if (pn <= 0 || (size_t)pn >= sizeof payload) return -1;
    return srv_job_receipt_publish(signer, payload, job->start_receipt_ref,
                                   job->start_receipt_hash);
}

static int srv_job_finalize_terminal(srv_job_t* job) {
    if (job->terminal_receipt_ready) return 0;
    if (job->receipt_failed) return -1;
    const jash_agent_process_result_t* result =
        jash_agent_process_get_result(job->process);
    if (!result || result->state != JASH_AGENT_PROCESS_DONE ||
        !result->quiescent) return 1;
    srv_job_release_input_pin(job);
    if (!result->terminal_status_valid || result->launcher_unexpected) {
        job->receipt_failed = 1;
        return -1;
    }

    if (srv_job_stream_to_cas(job->stdout_path, job->stdout_ref,
                              sizeof job->stdout_ref, job->stdout_hash,
                              &job->stdout_bytes, &job->stdout_dev,
                              &job->stdout_ino) != 0 ||
        srv_job_stream_to_cas(job->stderr_path, job->stderr_ref,
                              sizeof job->stderr_ref, job->stderr_hash,
                              &job->stderr_bytes, &job->stderr_dev,
                              &job->stderr_ino) != 0) {
        job->receipt_failed = 1;
        return -1;
    }
    job->terminal_receipt_ready = 1;
    return 0;
}

static int srv_job_attach_terminal_receipt(
        const srv_receipt_signer_t* signer, const srv_job_req_t* request,
        srv_job_t* job,
        char* response, size_t cap) {
    size_t response_len = strlen(response);
    if (response_len < 2 || response[response_len - 2] != '}' ||
        response[response_len - 1] != '\n') return -1;
    uint8_t digest[32];
    char response_hash[JASH_BLAKE3_HEX_LEN];
    jash_blake3(response, response_len, digest);
    jash_blake3_hex(digest, response_hash);

    char receipt_ref[224], receipt_hash[JASH_BLAKE3_HEX_LEN];
    int rn = snprintf(receipt_ref, sizeof receipt_ref,
                      "%s.%s.job-terminal.dsse", request->request_digest,
                      response_hash);
    if (rn <= 0 || (size_t)rn >= sizeof receipt_ref) return -1;
    const jash_agent_process_result_t* result =
        jash_agent_process_get_result(job->process);
    if (!result || result->state != JASH_AGENT_PROCESS_DONE ||
        !result->quiescent) return -1;
    char payload[4096], escaped_id[192];
    jash_json_esc(request->id_value, escaped_id, sizeof escaped_id);
    int pn = snprintf(payload, sizeof payload,
        "{\"schema\":\"jash.agent-job-receipt.v1\","
        "\"phase\":\"terminal\",\"request_id\":\"%s\","
        "\"request_blake3\":\"%s\",\"response_blake3\":\"%s\","
        "\"start_request_blake3\":\"%s\","
        "\"start_receipt_ref\":\"%s\","
        "\"start_receipt_blake3\":\"%s\",\"job_id\":\"%s\","
        "\"state\":\"done\",\"rc\":%d,\"signal\":%d,"
        "\"cancel_requested\":%s,\"kill_sent\":%s,"
        "\"output_limit\":%s,\"output_limit_bytes\":%llu,"
        "\"quiescent\":true,\"quiescence_forced\":%s,"
        "\"stdout_ref\":\"%s\","
        "\"stdout_blake3\":\"%s\",\"stdout_bytes\":%llu,"
        "\"stderr_ref\":\"%s\",\"stderr_blake3\":\"%s\","
        "\"stderr_bytes\":%llu}",
        escaped_id, request->request_digest, response_hash,
        job->request_digest, job->start_receipt_ref,
        job->start_receipt_hash, job->job_id, result->exit_code,
        result->term_signal, result->cancel_requested ? "true" : "false",
        result->kill_sent ? "true" : "false",
        result->output_limit_hit ? "true" : "false",
        (unsigned long long)result->output_limit_bytes,
        result->quiescence_forced ? "true" : "false", job->stdout_ref,
        job->stdout_hash, job->stdout_bytes, job->stderr_ref,
        job->stderr_hash, job->stderr_bytes);
    if (pn <= 0 || (size_t)pn >= sizeof payload ||
        srv_job_receipt_publish(signer, payload, receipt_ref,
                                receipt_hash) != 0) return -1;

    int suffix = snprintf(response + response_len - 2,
        cap - (response_len - 2),
        ",\"receipt_phase\":\"terminal\",\"receipt_ref\":\"%s\","
        "\"receipt_blake3\":\"%s\",\"receipt_pubkey\":\"%s\"}\n",
        receipt_ref, receipt_hash, job->receipt_pubkey);
    return suffix > 0 && (size_t)suffix < cap - (response_len - 2) ? 0 : -1;
}

static int srv_job_response(const srv_job_req_t* request, srv_job_t* job,
                            const srv_receipt_signer_t* signer,
                            const char* extra, char* out, size_t cap) {
    if (srv_job_format(request, job, extra, out, cap) != 0) return -1;
    if (!job->terminal_receipt_ready) return 0;
    if (job->receipt_failed ||
        srv_job_attach_terminal_receipt(signer, request, job, out, cap) != 0) {
        job->receipt_failed = 1;
        return -2;
    }
    return 0;
}

static int srv_job_finalize_if_done(int respfd, const srv_job_req_t* request,
                                    srv_job_t* job) {
    const jash_agent_process_result_t* result =
        jash_agent_process_get_result(job->process);
    if (!result) return -1;
    if (result->state != JASH_AGENT_PROCESS_DONE) return 0;
    if (!result->quiescent) {
        job->receipt_failed = 1;
        srv_dprintf(respfd,
                "{\"id\":%s,\"protocol_version\":2,\"op\":\"%s\","
                "\"error\":\"job not quiescent after DONE\","
                "\"outcome\":\"indeterminate\"}\n",
                request->id, request->op);
        return -1;
    }
    if (srv_job_finalize_terminal(job) == 0) return 0;
    srv_dprintf(respfd,
            "{\"id\":%s,\"protocol_version\":2,\"op\":\"%s\","
            "\"error\":\"terminal receipt commit failed\","
            "\"outcome\":\"indeterminate\"}\n",
            request->id, request->op);
    return -1;
}

static int srv_job_idem_begin(int respfd, const srv_job_req_t* request,
                              const char* text, srv_idem_t* reservation) {
    char* replay = NULL;
    int status = srv_idem_begin_id(request->id_value, text, reservation,
                                   &replay);
    if (status == SRV_IDEM_NEW) return 0;
    if (status == SRV_IDEM_REPLAY) {
        (void)srv_write_all(respfd, replay, strlen(replay));
        free(replay);
        return 1;
    }
    free(replay);
    if (status == SRV_IDEM_CONFLICT) {
        srv_job_error(respfd, request, "idempotency conflict");
    } else {
        srv_dprintf(respfd,
                "{\"id\":%s,\"protocol_version\":2,\"op\":\"%s\","
                "\"error\":\"request outcome unknown\","
                "\"outcome\":\"indeterminate\"}\n",
                request->id, request->op);
    }
    return -1;
}

static void srv_job_idem_reject_request(int respfd, srv_idem_t* reservation,
                                        const srv_job_req_t* request,
                                        const char* error) {
    char escaped[384], response[640];
    jash_json_esc(error, escaped, sizeof escaped);
    int n = snprintf(response, sizeof response,
                     "{\"id\":%s,\"protocol_version\":2,\"op\":\"%s\","
                     "\"error\":\"%s\"}\n",
                     request->id, request->op, escaped);
    if (n <= 0 || (size_t)n >= sizeof response) {
        if (reservation->fd >= 0) close(reservation->fd);
        srv_dprintf(respfd,
                "{\"id\":%s,\"protocol_version\":2,\"op\":\"%s\","
                "\"error\":\"mutation rejection overflow\","
                "\"outcome\":\"indeterminate\"}\n",
                request->id, request->op);
        return;
    }
    srv_idem_reject(respfd, reservation, request->id, response);
}

static void srv_job_idem_indeterminate(int respfd, srv_idem_t* reservation,
                                       const srv_job_req_t* request,
                                       const char* error,
                                       size_t accepted_bytes,
                                       int include_accepted) {
    if (reservation->fd >= 0) {
        close(reservation->fd);
        reservation->fd = -1;
    }
    char escaped[384];
    jash_json_esc(error, escaped, sizeof escaped);
    if (include_accepted)
        srv_dprintf(respfd,
                "{\"id\":%s,\"protocol_version\":2,\"op\":\"%s\","
                "\"error\":\"%s\",\"accepted_bytes\":%zu,"
                "\"outcome\":\"indeterminate\"}\n",
                request->id, request->op, escaped, accepted_bytes);
    else
        srv_dprintf(respfd,
                "{\"id\":%s,\"protocol_version\":2,\"op\":\"%s\","
                "\"error\":\"%s\",\"outcome\":\"indeterminate\"}\n",
                request->id, request->op, escaped);
}

static int srv_job_idem_commit(int respfd, srv_idem_t* reservation,
                               const srv_job_req_t* request,
                               const char* response) {
    int committed = srv_idem_complete(reservation, response) == 0;
    if (reservation->fd >= 0) {
        close(reservation->fd);
        reservation->fd = -1;
    }
    if (committed) {
        (void)srv_write_all(respfd, response, strlen(response));
        return 0;
    }
    srv_dprintf(respfd,
            "{\"id\":%s,\"protocol_version\":2,\"op\":\"%s\","
            "\"error\":\"idempotency commit failed\","
            "\"outcome\":\"indeterminate\"}\n",
            request->id, request->op);
    return -1;
}

static int srv_job_attach_action_receipt(
        const srv_receipt_signer_t* signer, const srv_job_req_t* request,
        const srv_job_t* job, const char* operational,
        char* response, size_t cap) {
    size_t response_len = strlen(response);
    if (response_len < 2 || response[response_len - 2] != '}' ||
        response[response_len - 1] != '\n') return -1;
    uint8_t digest[32];
    char response_hash[JASH_BLAKE3_HEX_LEN];
    jash_blake3(response, response_len, digest);
    jash_blake3_hex(digest, response_hash);

    char receipt_ref[192], receipt_hash[JASH_BLAKE3_HEX_LEN];
    int rn = snprintf(receipt_ref, sizeof receipt_ref,
                      "%s.job-action.dsse", request->request_digest);
    if (rn <= 0 || (size_t)rn >= sizeof receipt_ref) return -1;
    char payload[3072], escaped_id[192];
    jash_json_esc(request->id_value, escaped_id, sizeof escaped_id);
    int pn = snprintf(payload, sizeof payload,
        "{\"schema\":\"jash.agent-job-receipt.v1\",\"phase\":\"action\","
        "\"request_id\":\"%s\",\"request_blake3\":\"%s\","
        "\"response_blake3\":\"%s\",\"job_id\":\"%s\","
        "\"op\":\"%s\",\"start_request_blake3\":\"%s\","
        "\"start_receipt_ref\":\"%s\","
        "\"start_receipt_blake3\":\"%s\",%s}",
        escaped_id, request->request_digest, response_hash, job->job_id,
        request->op, job->request_digest, job->start_receipt_ref,
        job->start_receipt_hash, operational);
    if (pn <= 0 || (size_t)pn >= sizeof payload ||
        srv_job_receipt_publish(signer, payload, receipt_ref,
                                receipt_hash) != 0) return -1;
    int suffix = snprintf(response + response_len - 2,
        cap - (response_len - 2),
        ",\"receipt_phase\":\"action\",\"receipt_ref\":\"%s\","
        "\"receipt_blake3\":\"%s\",\"receipt_pubkey\":\"%s\"}\n",
        receipt_ref, receipt_hash, job->receipt_pubkey);
    return suffix > 0 && (size_t)suffix < cap - (response_len - 2) ? 0 : -1;
}

static int srv_job_env_entry_is(const char* entry, const char* name) {
    size_t length = strlen(name);
    return strncmp(entry, name, length) == 0 && entry[length] == '=';
}

static int srv_job_envp_add(char** envp, size_t* count, size_t* bytes,
                            const char* entry) {
    size_t length = strlen(entry) + 1u;
    if (*count >= JASH_AGENT_PROCESS_MAX_ENV ||
        length > JASH_AGENT_PROCESS_MAX_ENV_BYTES ||
        *bytes > JASH_AGENT_PROCESS_MAX_ENV_BYTES - length) {
        errno = E2BIG;
        return -1;
    }
    envp[*count] = strdup(entry);
    if (!envp[*count]) return -1;
    (*count)++;
    *bytes += length;
    return 0;
}

static void srv_job_envp_free(char** envp) {
    if (!envp) return;
    for (size_t i = 0; envp[i]; i++) free(envp[i]);
    free(envp);
}

static int srv_job_build_envp(const srv_job_req_t* request, char*** out,
                              char* error, size_t error_cap) {
    *out = NULL;
    char** envp = calloc(JASH_AGENT_PROCESS_MAX_ENV + 1u, sizeof *envp);
    if (!envp) {
        snprintf(error, error_cap, "cannot allocate job environment");
        return -1;
    }
    int emitted[SRV_JOB_ENV_MAX] = {0};
    size_t count = 0, bytes = 0;
    for (size_t i = 0; environ && environ[i]; i++) {
        if (srv_job_env_entry_is(environ[i], "JASH_AGENT_CANCEL_FD") ||
            srv_job_env_entry_is(environ[i], "JASH_AGENT_CGROUP_ROOT"))
            continue;
        if (request->resolved_cwd[0] &&
            srv_job_env_entry_is(environ[i], "PWD"))
            continue;
        int overlay = -1;
        for (int k = 0; k < request->env_n; k++)
            if (srv_job_env_entry_is(environ[i], request->env_k[k])) {
                overlay = k;
                break;
            }
        if (overlay >= 0) {
            if (emitted[overlay]) continue;
            size_t length = strlen(request->env_k[overlay]) +
                            strlen(request->env_v[overlay]) + 2u;
            char* entry = malloc(length);
            if (!entry) goto failed;
            snprintf(entry, length, "%s=%s", request->env_k[overlay],
                     request->env_v[overlay]);
            int added = srv_job_envp_add(envp, &count, &bytes, entry);
            free(entry);
            if (added != 0) goto failed;
            emitted[overlay] = 1;
            continue;
        }
        if (srv_job_envp_add(envp, &count, &bytes, environ[i]) != 0)
            goto failed;
    }
    for (int k = 0; k < request->env_n; k++) {
        if (emitted[k]) continue;
        size_t length = strlen(request->env_k[k]) + strlen(request->env_v[k]) + 2u;
        char* entry = malloc(length);
        if (!entry) goto failed;
        snprintf(entry, length, "%s=%s", request->env_k[k], request->env_v[k]);
        int added = srv_job_envp_add(envp, &count, &bytes, entry);
        free(entry);
        if (added != 0) goto failed;
    }
    if (request->resolved_cwd[0]) {
        /* This is the logical name captured when cwd_fd was opened. The
         * inode-pinned working directory can outlive a later path rename. */
        size_t length = strlen(request->resolved_cwd) + 5u;
        char* entry = malloc(length);
        if (!entry) goto failed;
        snprintf(entry, length, "PWD=%s", request->resolved_cwd);
        int added = srv_job_envp_add(envp, &count, &bytes, entry);
        free(entry);
        if (added != 0) goto failed;
    }
    *out = envp;
    return 0;

failed:
    snprintf(error, error_cap, "%s",
             errno == E2BIG ? "effective job environment exceeds its limit"
                            : "cannot construct effective job environment");
    srv_job_envp_free(envp);
    return -1;
}

static int srv_job_stdin_fd(const srv_job_req_t* request, const char* directory,
                            char* error, size_t error_cap) {
    if (!(request->seen & SRV_JOB_SEEN_STDIN_B64)) return -1;
    char path[1100];
    int length = snprintf(path, sizeof path, "%s/.stdin-XXXXXX", directory);
    if (length <= 0 || (size_t)length >= sizeof path) {
        snprintf(error, error_cap, "stdin path exceeds its limit");
        errno = ENAMETOOLONG;
        return -2;
    }
    int fd = srv_cloexec(mkstemp(path));
    if (fd < 0) {
        snprintf(error, error_cap, "cannot create anonymous stdin");
        return -2;
    }
    if (unlink(path) != 0) {
        int saved = errno;
        close(fd);
        (void)unlink(path);
        errno = saved;
        snprintf(error, error_cap, "cannot anonymize stdin file");
        return -2;
    }
    size_t offset = 0;
    while (offset < request->stdin_len) {
        ssize_t written = write(fd, request->stdin_data + offset,
                                request->stdin_len - offset);
        if (written > 0) offset += (size_t)written;
        else if (written < 0 && errno == EINTR) continue;
        else break;
    }
    if (offset != request->stdin_len || lseek(fd, 0, SEEK_SET) != 0) {
        close(fd);
        snprintf(error, error_cap, "cannot prepare complete stdin");
        return -2;
    }
    return fd;
}

typedef struct {
    int fd;
    int durable;
    int stream;
    srv_data_buffer_t* volatile_object;
    uint64_t size;
    uint64_t materialized_bytes;
    uint64_t verified_bytes;
    uint64_t stream_offset;
    char ref[160];
    char hash[JASH_BLAKE3_HEX_LEN];
    char stream_ref[64];
    char stream_final_ref[160];
    char stream_final_hash[JASH_BLAKE3_HEX_LEN];
} srv_job_input_t;

static int srv_job_active_snapshot_bytes(srv_job_table_t* table,
                                         uint64_t* bytes) {
    *bytes = 0;
    for (int i = 0; i < SRV_JOB_MAX; i++) {
        srv_job_t* job = &table->jobs[i];
        if (!job->used || !job->stdin_materialized_bytes) continue;
        if (srv_job_refresh(job) != 0) return -1;
        const jash_agent_process_result_t* result =
            jash_agent_process_get_result(job->process);
        if (result && result->state == JASH_AGENT_PROCESS_DONE &&
            result->quiescent) continue;
        if (*bytes > UINT64_MAX - job->stdin_materialized_bytes) return -1;
        *bytes += job->stdin_materialized_bytes;
    }
    return 0;
}

static int srv_job_input_open(srv_job_table_t* table,
                              const srv_job_req_t* request,
                              srv_job_input_t* input,
                              char* error, size_t error_cap) {
    memset(input, 0, sizeof *input);
    input->fd = -1;
    if (!(request->seen & (SRV_JOB_SEEN_ARTIFACT_REF |
                           SRV_JOB_SEEN_STREAM_REF))) return 0;

    if (request->seen & SRV_JOB_SEEN_STREAM_REF) {
        srv_data_stream_t* stream = srv_stream_find(request->stream_ref);
        if (!stream) {
            snprintf(error, error_cap, "stream_unavailable");
            return -1;
        }
        if (stream->in_flight || stream->offset != stream->acknowledged) {
            snprintf(error, error_cap, "stream_has_in_flight_bytes");
            return -1;
        }
        srv_job_t* source_job = srv_job_find(table, stream->job_id);
        if (!source_job || srv_job_refresh(source_job) != 0) {
            snprintf(error, error_cap, "stream_source_unavailable");
            return -1;
        }
        const jash_agent_process_result_t* result =
            jash_agent_process_get_result(source_job->process);
        if (!result || result->state != JASH_AGENT_PROCESS_DONE ||
            !result->quiescent) {
            snprintf(error, error_cap, "stream_not_terminal");
            return -1;
        }
        if (srv_job_finalize_terminal(source_job) != 0) {
            snprintf(error, error_cap, "stream_source_unavailable");
            return -1;
        }
        int source = -1, terminal = 0;
        uint64_t source_size = 0;
        unsigned final_flags = 0;
        const char *final_ref = NULL, *final_hash = NULL;
        const char* source_error = srv_stream_source(
            source_job, stream->stream, 0, &source, &source_size, &terminal,
            &final_flags, &final_ref, &final_hash);
        if (source_error || !terminal) {
            if (source >= 0) close(source);
            snprintf(error, error_cap, "%s",
                     source_error ? source_error : "stream_not_terminal");
            return -1;
        }
        if (final_flags & JASH_IPC_STREAM_INCOMPLETE) {
            close(source);
            snprintf(error, error_cap, "stream_incomplete");
            return -1;
        }
        if (stream->offset > source_size) {
            close(source);
            snprintf(error, error_cap, "stream_range_invalid");
            return -1;
        }
        srv_data_policy_t policy;
        uint64_t length = source_size - stream->offset;
        if (srv_data_policy_load(&policy) != 0) {
            close(source);
            snprintf(error, error_cap, "policy_invalid");
            return -1;
        }
        uint64_t active_snapshots = 0;
        if (srv_job_active_snapshot_bytes(table, &active_snapshots) != 0) {
            close(source);
            snprintf(error, error_cap, "stdin_snapshot_accounting_failed");
            return -1;
        }
        if (source_size > policy.max_bytes ||
            active_snapshots > policy.max_bytes - length) {
            close(source);
            snprintf(error, error_cap, "stream_input_too_large");
            return -1;
        }
        srv_stream_hash_t hash;
        jash_buffer_copy_stats copied;
        blake3_init(&hash.full);
        blake3_init(&hash.range);
        hash.offset=stream->offset;
        hash.length=length;
        int range=jash_buffer_copy_observed_range_fd(source,source_size,
            stream->offset,length,policy.max_bytes,srv_stream_hash_chunk,&hash,&copied);
        close(source);
        if(range<0) {
            snprintf(error,error_cap,"stream_snapshot_failed");
            return -1;
        }
        uint8_t digest[32];
        char snapshot_hash[JASH_BLAKE3_HEX_LEN];
        blake3_final(&hash.full,digest,sizeof(digest));
        jash_blake3_hex(digest,snapshot_hash);
        blake3_final(&hash.range,digest,sizeof(digest));
        jash_blake3_hex(digest,input->hash);
        if(strcmp(snapshot_hash,final_hash)||copied.read_bytes!=source_size||
           copied.copied_bytes!=length) {
            close(range);
            snprintf(error,error_cap,"stream_verification_failed");
            return -1;
        }
        input->fd = jash_buffer_reader_fd(range, length, policy.max_bytes);
        close(range);
        if (input->fd < 0) {
            snprintf(error, error_cap, "stream_reader_unavailable");
            return -1;
        }
        input->stream = 1;
        input->size = length;
        input->materialized_bytes = copied.copied_bytes;
        input->verified_bytes = copied.read_bytes;
        input->stream_offset = stream->offset;
        snprintf(input->stream_ref, sizeof input->stream_ref, "%s", stream->ref);
        snprintf(input->stream_final_ref, sizeof input->stream_final_ref,
                 "%s", final_ref);
        snprintf(input->stream_final_hash, sizeof input->stream_final_hash,
                 "%s", final_hash);
        return 0;
    }

    snprintf(input->ref, sizeof input->ref, "%s", request->artifact_ref);
    if (!strncmp(request->artifact_ref, "v-", 2)) {
        srv_data_buffer_t* object = srv_data_find(request->artifact_ref);
        if (!object) {
            snprintf(error, error_cap, "volatile_ref_unavailable");
            return -1;
        }
        input->fd = jash_buffer_reader_fd(object->fd, object->size,
                                          SRV_DATA_MAX_BYTES);
        if (input->fd < 0) {
            snprintf(error, error_cap, "artifact_reader_unavailable");
            return -1;
        }
        input->volatile_object = object;
        input->size = object->size;
        snprintf(input->hash, sizeof input->hash, "%s", object->hash);
        return 0;
    }

    char expected_hash[65];
    uint64_t expected_size;
    srv_data_policy_t policy;
    if (srv_data_policy_load(&policy) != 0) {
        snprintf(error, error_cap, "policy_invalid");
        return -1;
    }
    if (strncmp(request->artifact_ref, "a:", 2) ||
        !store_cas_name(request->artifact_ref + 2, expected_hash,
                        &expected_size) ||
        expected_size > policy.max_bytes) {
        snprintf(error, error_cap, "artifact_ref_invalid");
        return -1;
    }
    uint64_t active_snapshots = 0;
    if (srv_job_active_snapshot_bytes(table, &active_snapshots) != 0) {
        snprintf(error, error_cap, "stdin_snapshot_accounting_failed");
        return -1;
    }
    if (active_snapshots > policy.max_bytes - expected_size) {
        snprintf(error, error_cap, "artifact_snapshot_quota_exceeded");
        return -1;
    }
    char path[1100];
    int n = snprintf(path, sizeof path, "%s/.jash/agent_artifacts/%s",
                     srv_home(), request->artifact_ref + 2);
    struct stat before;
    int source = n > 0 && (size_t)n < sizeof path
        ? srv_open_regular_nofollow(path, &before) : -1;
    if (source < 0 || before.st_size < 0 ||
        (uint64_t)before.st_size != expected_size) {
        if (source >= 0) close(source);
        snprintf(error, error_cap, "artifact_verification_failed");
        return -1;
    }
    int snapshot = jash_buffer_copy_fd(source, policy.max_bytes);
    close(source);
    if (snapshot < 0 || lseek(snapshot, 0, SEEK_SET) != 0) {
        if (snapshot >= 0) close(snapshot);
        snprintf(error, error_cap, "artifact_snapshot_failed");
        return -1;
    }
    char actual_hash[JASH_BLAKE3_HEX_LEN];
    if (srv_hash_regular_fd(snapshot, before.st_size, actual_hash) != 0 ||
        strcmp(actual_hash, expected_hash)) {
        close(snapshot);
        snprintf(error, error_cap, "artifact_verification_failed");
        return -1;
    }
    input->fd = jash_buffer_reader_fd(snapshot, expected_size, policy.max_bytes);
    close(snapshot);
    if (input->fd < 0) {
        snprintf(error, error_cap, "artifact_reader_unavailable");
        return -1;
    }
    input->durable = 1;
    input->size = expected_size;
    input->materialized_bytes = expected_size;
    input->verified_bytes = expected_size;
    snprintf(input->hash, sizeof input->hash, "%s", expected_hash);
    return 0;
}

static void srv_job_create(int respfd, srv_job_table_t* table,
                           const srv_job_req_t* request, const char* text,
                           const srv_receipt_signer_t* signer) {
    char digest[JASH_BLAKE3_HEX_LEN];
    srv_job_request_digest(text, digest);
    srv_job_t* prior = srv_job_find_request(table, request->id_value);
    if (prior) {
        if (strcmp(prior->request_digest, digest) != 0) {
            srv_job_error(respfd, request, "idempotency conflict");
            return;
        }
        if (!prior->create_response_ready) {
            srv_dprintf(respfd,
                    "{\"id\":%s,\"protocol_version\":2,\"op\":\"%s\","
                    "\"error\":\"request outcome unknown\","
                    "\"outcome\":\"indeterminate\"}\n",
                    request->id, request->op);
            return;
        }
        (void)srv_write_all(respfd, prior->create_response,
                            strlen(prior->create_response));
        return;
    }

    srv_idem_t reservation;
    char* persisted_replay = NULL;
    int idem_status = srv_idem_begin_id(request->id_value, text, &reservation,
                                        &persisted_replay);
    free(persisted_replay);
    if (idem_status != SRV_IDEM_NEW) {
        if (idem_status == SRV_IDEM_CONFLICT)
            srv_job_error(respfd, request, "idempotency conflict");
        else
            srv_dprintf(respfd,
                    "{\"id\":%s,\"protocol_version\":2,\"op\":\"%s\","
                    "\"error\":\"request outcome unknown\","
                    "\"outcome\":\"indeterminate\"}\n",
                    request->id, request->op);
        return;
    }

    srv_job_t* slot = srv_job_free_slot(table);
    if (!slot) {
        srv_job_idem_reject_request(respfd, &reservation, request,
                                    "job table full");
        return;
    }
    char job_id[SRV_JOB_ID_MAX];
    if (srv_job_random_id(table, job_id) != 0) {
        srv_job_idem_reject_request(respfd, &reservation, request,
                                    "opaque job id generation failed");
        return;
    }
    char dir[896];
    if (srv_private_dir("agent_jobs", dir, sizeof dir) != 0) {
        srv_job_idem_reject_request(respfd, &reservation, request,
                                    "private job directory unavailable");
        return;
    }
    char stdout_path[1100], stderr_path[1100];
    int on = snprintf(stdout_path, sizeof stdout_path, "%s/%s.stdout", dir, job_id);
    int en = snprintf(stderr_path, sizeof stderr_path, "%s/%s.stderr", dir, job_id);
    if (on <= 0 || (size_t)on >= sizeof stdout_path ||
        en <= 0 || (size_t)en >= sizeof stderr_path) {
        srv_job_idem_reject_request(respfd, &reservation, request,
                                    "job spool path too long");
        return;
    }

    char** envp = NULL;
    char context_error[256];
    if (srv_job_build_envp(request, &envp, context_error,
                           sizeof context_error) != 0) {
        if (srv_idem_abort(&reservation) != 0)
            srv_dprintf(respfd,
                    "{\"id\":%s,\"protocol_version\":2,\"op\":\"%s\","
                    "\"error\":\"idempotency abort failed\","
                    "\"outcome\":\"indeterminate\"}\n",
                    request->id, request->op);
        else
            srv_job_error(respfd, request, context_error);
        return;
    }
    srv_job_input_t input;
    int input_status = srv_job_input_open(table, request, &input, context_error,
                                          sizeof context_error);
    int stdin_fd = input_status == 0 && input.fd >= 0 ? input.fd
        : srv_job_stdin_fd(request, dir, context_error, sizeof context_error);
    if (input_status != 0 ||
        ((request->seen & SRV_JOB_SEEN_STDIN_B64) && stdin_fd < 0)) {
        srv_job_envp_free(envp);
        if (srv_idem_abort(&reservation) != 0)
            srv_dprintf(respfd,
                    "{\"id\":%s,\"protocol_version\":2,\"op\":\"%s\","
                    "\"error\":\"idempotency abort failed\","
                    "\"outcome\":\"indeterminate\"}\n",
                    request->id, request->op);
        else
            srv_job_error(respfd, request, context_error);
        return;
    }

    jash_agent_process_options_t options;
    jash_agent_process_options_init(&options);
    int session_fds[9 + SRV_DATA_SLOTS] = {0, 1, 2, respfd, table->input_fd, table->cancel_fd, srv_shutdown_fd, srv_store_lease, srv_data_fd};
    for (int i = 0; i < SRV_DATA_SLOTS; i++) session_fds[9 + i] = srv_data_buffers[i].used ? srv_data_buffers[i].fd : -1;
    options.close_in_launcher = session_fds;
    options.close_in_launcher_count = sizeof(session_fds) / sizeof(session_fds[0]);
    options.stdout_path = stdout_path;
    options.stderr_path = stderr_path;
    options.cwd_fd = request->cwd_fd;
    options.envp = (const char* const*)envp;
    options.stdin_fd = stdin_fd;
    options.stdin_pipe = request->stdin_pipe;
    if (request->seen & SRV_JOB_SEEN_RESOURCES) {
        options.resources = &request->resources;
        options.cgroup_parent_fd = table->cgroup_parent_fd;
    }
    int background = strcmp(request->op, "job.start") == 0;
    /* The supervisor must return after launch so phase=start is committed
     * while the request is nonterminal. Foreground exec waits below only
     * after that receipt exists. */
    options.background = 1;
    options.pty = request->pty;
    options.pty_rows = request->rows;
    options.pty_cols = request->cols;
    jash_agent_process_t* process = NULL;
    char process_error[256];
    if (jash_agent_process_start(&request->ir, &options, &process,
                                 process_error, sizeof process_error) != 0) {
        if (stdin_fd >= 0) close(stdin_fd);
        srv_job_envp_free(envp);
        int abort_failed = srv_idem_abort(&reservation) != 0;
        unlink(stdout_path);
        unlink(stderr_path);
        if (abort_failed)
            srv_dprintf(respfd,
                    "{\"id\":%s,\"protocol_version\":2,\"op\":\"%s\","
                    "\"error\":\"idempotency abort failed\","
                    "\"outcome\":\"indeterminate\"}\n",
                    request->id, request->op);
        else
            srv_job_error(respfd, request, process_error);
        return;
    }
    if (stdin_fd >= 0) close(stdin_fd);
    srv_job_envp_free(envp);

    memset(slot, 0, sizeof *slot);
    slot->used = 1;
    slot->process = process;
    slot->reservation = reservation;
    slot->background = background;
    slot->pty = request->pty;
    slot->stdin_pipe = request->stdin_pipe;
    if (request->seen & (SRV_JOB_SEEN_ARTIFACT_REF |
                         SRV_JOB_SEEN_STREAM_REF)) {
        slot->stdin_artifact_bytes = input.size;
        slot->stdin_materialized_bytes = input.materialized_bytes;
        slot->stdin_verified_bytes = input.verified_bytes;
        snprintf(slot->stdin_artifact_hash, sizeof slot->stdin_artifact_hash,
                 "%s", input.hash);
        if (input.stream) {
            slot->stdin_stream = 1;
            slot->stdin_stream_offset = input.stream_offset;
            snprintf(slot->stdin_stream_ref, sizeof slot->stdin_stream_ref,
                     "%s", input.stream_ref);
            snprintf(slot->stdin_stream_final_ref,
                     sizeof slot->stdin_stream_final_ref,
                     "%s", input.stream_final_ref);
            snprintf(slot->stdin_stream_final_hash,
                     sizeof slot->stdin_stream_final_hash,
                     "%s", input.stream_final_hash);
        } else {
            slot->stdin_artifact = 1;
            slot->stdin_artifact_durable = input.durable;
            snprintf(slot->stdin_artifact_ref, sizeof slot->stdin_artifact_ref,
                     "%s", input.ref);
            if (input.volatile_object) {
                input.volatile_object->readers++;
                slot->stdin_artifact_pin = 1;
            }
        }
    }
    snprintf(slot->job_id, sizeof slot->job_id, "%s", job_id);
    snprintf(slot->request_id, sizeof slot->request_id, "%s", request->id_value);
    snprintf(slot->request_digest, sizeof slot->request_digest, "%s", digest);
    snprintf(slot->stdout_path, sizeof slot->stdout_path, "%s", stdout_path);
    snprintf(slot->stderr_path, sizeof slot->stderr_path, "%s", stderr_path);
    snprintf(slot->receipt_pubkey, sizeof slot->receipt_pubkey, "%s",
             signer->pubkey_hex);

    char start_base[2048], resource_fields[768], input_fields[768];
    int resource_len = srv_job_resource_fields(jash_agent_process_get_result(process), 0,
                                               resource_fields, sizeof resource_fields);
    int input_len = srv_job_input_fields(slot, input_fields, sizeof input_fields);
    int start_len = snprintf(start_base, sizeof start_base,
            "{\"id\":%s,\"protocol_version\":2,\"op\":\"%s\","
            "\"job_id\":\"%s\",\"accepted\":true,\"state\":\"running\","
            "\"terminal\":false,\"background\":%s,\"pty\":%s%s%s}\n",
            request->id, request->op, slot->job_id,
            background ? "true" : "false", request->pty ? "true" : "false",
            resource_fields, input_fields);
    if (resource_len < 0 || input_len < 0 || start_len <= 2 ||
        (size_t)start_len >= sizeof start_base ||
        srv_job_publish_start_receipt(signer, request, slot, start_base) != 0) {
        jash_agent_process_destroy(slot->process);
        srv_job_release_input_pin(slot);
        unlink(slot->stdout_path);
        unlink(slot->stderr_path);
        if (slot->reservation.fd >= 0) close(slot->reservation.fd);
        memset(slot, 0, sizeof *slot);
        srv_dprintf(respfd,
                "{\"id\":%s,\"protocol_version\":2,\"op\":\"%s\","
                "\"error\":\"start receipt commit failed\","
                "\"outcome\":\"indeterminate\"}\n",
                request->id, request->op);
        return;
    }
    memcpy(slot->create_response, start_base, (size_t)start_len - 2u);
    char receipt_fields[1024];
    int final_len = snprintf(receipt_fields, sizeof receipt_fields,
        ",\"receipt_phase\":\"start\",\"receipt_ref\":\"%s\","
        "\"receipt_blake3\":\"%s\",\"receipt_pubkey\":\"%s\"}\n",
        slot->start_receipt_ref, slot->start_receipt_hash,
        slot->receipt_pubkey);
    if (final_len <= 0 || (size_t)final_len >= sizeof receipt_fields || (size_t)final_len >=
            sizeof slot->create_response - ((size_t)start_len - 2u)) {
        jash_agent_process_destroy(slot->process);
        srv_job_release_input_pin(slot);
        unlink(slot->stdout_path);
        unlink(slot->stderr_path);
        if (slot->reservation.fd >= 0) close(slot->reservation.fd);
        memset(slot, 0, sizeof *slot);
        srv_dprintf(respfd,
                "{\"id\":%s,\"protocol_version\":2,\"op\":\"%s\","
                "\"error\":\"start response commit failed\","
                "\"outcome\":\"indeterminate\"}\n",
                request->id, request->op);
        return;
    }
    memcpy(slot->create_response + start_len - 2u, receipt_fields, (size_t)final_len + 1);
    if (!background) {
        int waited;
        do { waited = jash_agent_process_wait(slot->process, srv_shutdown_fd >= 0 ? 50 : -1); }
        while (waited == 1 && !srv_shutdown_requested());
        if (srv_shutdown_requested()) return;
        if (waited != 0) {
            jash_agent_process_destroy(slot->process);
            srv_job_release_input_pin(slot);
            slot->process = NULL;
            slot->receipt_failed = 1;
            srv_dprintf(respfd,
                    "{\"id\":%s,\"protocol_version\":2,\"op\":\"exec\","
                    "\"error\":\"foreground wait failed\","
                    "\"outcome\":\"indeterminate\"}\n", request->id);
            return;
        }
        int finalized = srv_job_finalize_terminal(slot);
        if (finalized != 0) {
            if (slot->process) jash_agent_process_destroy(slot->process);
            slot->process = NULL;
            slot->receipt_failed = 1;
            srv_dprintf(respfd,
                    "{\"id\":%s,\"protocol_version\":2,\"op\":\"exec\","
                    "\"error\":\"terminal receipt commit failed\","
                    "\"outcome\":\"indeterminate\"}\n", request->id);
            return;
        }
        char response[4096];
        if (srv_job_response(request, slot, signer, NULL,
                             response, sizeof response) != 0) {
            srv_dprintf(respfd,
                    "{\"id\":%s,\"protocol_version\":2,\"op\":\"exec\","
                    "\"error\":\"terminal response commit failed\","
                    "\"outcome\":\"indeterminate\"}\n", request->id);
        } else {
            snprintf(slot->create_response, sizeof slot->create_response,
                     "%s", response);
            slot->create_response_ready = 1;
            (void)srv_write_all(respfd, response, strlen(response));
        }
        return;
    }
    slot->create_response_ready = 1;
    (void)srv_write_all(respfd, slot->create_response,
                        strlen(slot->create_response));
}

/* One immutable local observation per job/session end. The checksum detects
 * corruption; the inner request/response/receipt provide independent binding. */
typedef struct {
    char magic[8];
    uint32_t request_len, response_len;
    char job_id[35];
    unsigned char digest[32];
} srv_job_archive_t;
#define SRV_JOB_ARCHIVE_MAX 8192

static int srv_job_archive_id(const char* id) {
    if (strlen(id) != 34 || strncmp(id, "j-", 2)) return 0;
    for (int i = 2; i < 34; i++) if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f'))) return 0;
    return 1;
}

static int srv_job_archive_write_impl(srv_job_t* job, const char* request, const char* response) {
    srv_job_archive_t header; memset(&header, 0, sizeof(header)); memcpy(header.magic, "JASHJA01", 8);
    size_t a = strlen(request) + 1, b = strlen(response) + 1;
    if (!srv_job_archive_id(job->job_id) || a + b > SRV_JOB_ARCHIVE_MAX) return -1;
    header.request_len = (uint32_t)a; header.response_len = (uint32_t)b;
    /* The archive ID validator requires exactly 34 bytes plus NUL. */
    memcpy(header.job_id, job->job_id, sizeof(header.job_id));
    unsigned char bytes[sizeof(header) + SRV_JOB_ARCHIVE_MAX];
    memcpy(bytes, &header, sizeof(header)); memcpy(bytes + sizeof(header), request, a);
    memcpy(bytes + sizeof(header) + a, response, b);
    jash_blake3(bytes, sizeof(header) + a + b, header.digest); memcpy(bytes, &header, sizeof(header));
    char dir[896], path[1100], temporary[1100];
    if (srv_private_dir("agent_job_results", dir, sizeof(dir))) return -1;
    int n = snprintf(path, sizeof(path), "%s/%s.result", dir, job->job_id);
    int t = snprintf(temporary, sizeof(temporary), "%s/.%s.%ld.tmp", dir, job->job_id, (long)getpid());
    if (n < 0 || (size_t)n >= sizeof(path) || t < 0 || (size_t)t >= sizeof(temporary)) return -1;
    int fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    struct stat owned = {0};
    int ok = !fstat(fd, &owned) && !fchmod(fd, 0600) &&
        !srv_write_all(fd, (const char*)bytes, sizeof(header) + a + b) && !fsync(fd) && !fstat(fd, &owned);
    if (close(fd)) ok = 0;
    if (!ok || !srv_regular_identity_ok(temporary, &owned) ||
        jash_osif_renameat2(AT_FDCWD, temporary, AT_FDCWD, path, 1u) ||
        !srv_regular_identity_ok(path, &owned) || srv_fsync_dir(dir)) {
        srv_unlink_if_identity(temporary, &owned); return -1;
    }
    return 0;
}

static int srv_job_archive_write(srv_job_t* job, const char* request, const char* response) {
    int admission;
    if (srv_store_admit(sizeof(srv_job_archive_t) + strlen(request) + strlen(response) + 2, NULL, &admission)) return -1;
    int rc = srv_job_archive_write_impl(job, request, response);
    if (admission >= 0) close(admission);
    return rc;
}

static int srv_job_archive_read(const char* job_id, char bytes[SRV_JOB_ARCHIVE_MAX], const char** request, const char** response) {
    if (!srv_job_archive_id(job_id)) return -1;
    const char* home = srv_home(); char dir[896], path[1100];
    int d = home ? snprintf(dir, sizeof(dir), "%s/.jash/agent_job_results", home) : -1;
    if (d < 0 || (size_t)d >= sizeof(dir) || srv_secure_private_dir(dir)) return -1;
    int n = snprintf(path, sizeof(path), "%s/%s.result", dir, job_id);
    if (n < 0 || (size_t)n >= sizeof(path)) return -1;
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return -1;
    struct stat st; srv_job_archive_t header;
    unsigned char all[sizeof(header) + SRV_JOB_ARCHIVE_MAX]; size_t used = 0;
    int ok = !fstat(fd, &st) && srv_regular_identity_ok(path, &st) && st.st_size > (off_t)sizeof(header) &&
        (uintmax_t)st.st_size <= sizeof(all);
    while (ok && used < (size_t)st.st_size) {
        ssize_t r = read(fd, all + used, (size_t)st.st_size - used);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) { ok = 0; break; } used += (size_t)r;
    }
    if (ok) {
        char extra; ssize_t trailing;
        do { trailing = read(fd, &extra, 1); } while (trailing < 0 && errno == EINTR);
        if (trailing != 0) ok = 0;
    }
    if (close(fd)) ok = 0;
    if (!ok) return -1;
    memcpy(&header, all, sizeof(header));
    size_t a = header.request_len, b = header.response_len;
    if (memcmp(header.magic, "JASHJA01", 8) || memcmp(header.job_id, job_id, 35) || !a || !b ||
        a > SRV_JOB_ARCHIVE_MAX || b > SRV_JOB_ARCHIVE_MAX - a || used != sizeof(header) + a + b) return -1;
    unsigned char expected[32]; memcpy(expected, header.digest, 32); memset(header.digest, 0, 32);
    memcpy(all, &header, sizeof(header)); jash_blake3(all, used, header.digest);
    if (memcmp(expected, header.digest, 32)) return -1;
    memcpy(bytes, all + sizeof(header), a + b);
    if (bytes[a - 1] || bytes[a + b - 1] || strlen(bytes) + 1 != a || strlen(bytes + a) + 1 != b ||
        !jash_json_utf8_valid(bytes) || !jash_json_utf8_valid(bytes + a)) return -1;
    *request = bytes; *response = bytes + a; return 0;
}

static int maintenance_root_released(const char*, const char*, const char*, const srv_receipt_signer_t*, uint64_t*);

static void srv_job_result(int respfd, const srv_job_req_t* request, const srv_receipt_signer_t* signer) {
    char bytes[SRV_JOB_ARCHIVE_MAX]; const char *observed_request = NULL, *observed_response = NULL;
    int available = !srv_job_archive_read(request->job_id, bytes, &observed_request, &observed_response);
    jash_json_writer_t out; jash_jsonw_init(&out, jash_jsonw_fd_sink, &respfd);
    jash_jsonw_raw(&out, "{\"schema\":\"jash.agent-job-result.v1\",\"id\":"); jash_jsonw_string(&out, request->id_value);
    jash_jsonw_raw(&out, ",\"protocol_version\":2,\"op\":\"job.result\",\"job_id\":"); jash_jsonw_string(&out, request->job_id);
    jash_jsonw_raw(&out, ",\"available\":"); jash_jsonw_raw(&out, available ? "true" : "false");
    const char* retention = "unknown";
    if (available) {
        char path[1400], name[96], hash[65]; struct stat st; uint64_t budget = 0;
        snprintf(name, sizeof(name), "%s.result", request->job_id);
        snprintf(path, sizeof(path), "%s/.jash/agent_job_results/%s", srv_home(), name);
        int fd = srv_open_regular_nofollow(path, &st);
        if (fd >= 0 && !srv_hash_regular_fd(fd, st.st_size, hash)) {
            int released = maintenance_root_released("agent_job_results", name, hash, signer, &budget);
            retention = released < 0 ? "unknown" : released ? "released" : "kept";
        }
        if (fd >= 0) close(fd);
    }
    jash_jsonw_raw(&out, ",\"retention\":"); jash_jsonw_string(&out, retention);
    jash_jsonw_raw(&out, ",\"origin\":\"runtime-session-end\",\"request_json\":");
    if (available) jash_jsonw_string(&out, observed_request); else jash_jsonw_raw(&out, "null");
    jash_jsonw_raw(&out, ",\"response_json\":");
    if (available) jash_jsonw_string(&out, observed_response); else jash_jsonw_raw(&out, "null");
    jash_jsonw_raw(&out, ",\"error\":"); jash_jsonw_raw(&out, available ? "null" : "\"archive_unavailable\"");
    jash_jsonw_raw(&out, "}\n"); (void)jash_jsonw_finish(&out);
}

static int srv_job_archive_terminal(srv_job_t* job, const srv_receipt_signer_t* signer, const char* reason) {
    if (srv_job_finalize_terminal(job)) return -1;
    srv_job_req_t* observation = calloc(1, sizeof(*observation));
    if (!observation) return -1;
    char request[256], response[4096], extra[192];
    snprintf(observation->id_value, sizeof(observation->id_value), "end:%s", job->job_id);
    snprintf(observation->id, sizeof(observation->id), "\"%s\"", observation->id_value);
    /* `observation` is heap-owned: blibc cannot infer the bound of this
     * interior field for strcpy. Supply the field's actual capacity. */
    snprintf(observation->op, sizeof(observation->op), "%s", "job.get");
    int n = snprintf(request, sizeof(request), "{\"id\":%s,\"protocol_version\":2,\"op\":\"job.get\",\"job_id\":\"%s\",\"caps\":[\"job:read\"]}", observation->id, job->job_id);
    if (n < 0 || (size_t)n >= sizeof(request)) { free(observation); return -1; }
    srv_job_request_digest(request, observation->request_digest);
    snprintf(extra, sizeof(extra), "\"observation_origin\":\"runtime-session-end\",\"completion_trigger\":\"%s\"", reason);
    int rc = srv_job_response(observation, job, signer, extra, response, sizeof(response));
    if (!rc) rc = srv_job_archive_write(job, request, response);
    free(observation); return rc;
}

enum { STORE_CAS, STORE_HOLD, STORE_STAGE, STORE_ARCHIVE, STORE_RECEIPT, STORE_REQUEST, STORE_SPOOL, STORE_OTHER };
static const char* const store_dirs[] = {"overflow", "agent_job_results", "agent_receipts", "agent_requests", "agent_jobs"};
typedef struct {
    char name[256], reference[160];
    char root_hash[65];
    struct stat st;
    int directory, kind, referenced, metadata_valid, held, released;
    const char* action;
} store_entry_t;
typedef struct {
    store_entry_t* entries;
    size_t count;
    int fds[5], complete, changed, suppress_refs;
    uint64_t maintenance_bytes;
    const char* root_error;
    uint64_t root_bytes, checked_bytes, reclaimed, reclaimed_bytes, planned, reconciled;
} store_catalog_t;

static int store_cas_name(const char* name, char hash[65], uint64_t* size) {
    if (strncmp(name, "blake3-", 7) || strlen(name) < 77 || strlen(name) >= 160) return 0;
    for (int i = 0; i < 64; i++) {
        char c = name[7 + i]; if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return 0;
        hash[i] = c;
    }
    hash[64] = 0;
    const char* p = name + 71; if (*p++ != '-' || !isdigit((unsigned char)*p)) return 0;
    if (*p == '0' && isdigit((unsigned char)p[1])) return 0;
    uint64_t value = 0;
    do {
        unsigned digit = (unsigned)(*p++ - '0');
        if (value > (UINT64_MAX - digit) / 10) return 0;
        value = value * 10 + digit;
    } while (isdigit((unsigned char)*p));
    if (strcmp(p, ".bin")) return 0;
    *size = value; return 1;
}

static void store_mark_refs(store_catalog_t* catalog, const char* text) {
    if (catalog->suppress_refs) return;
    const char* p = text;
    while ((p = strstr(p, "blake3-"))) {
        char name[160]; size_t n = 0;
        while (p[n] && (isalnum((unsigned char)p[n]) || p[n] == '-' || p[n] == '.') && n < sizeof(name) - 1) n++;
        memcpy(name, p, n); name[n] = 0;
        char hash[65]; uint64_t bytes;
        if (store_cas_name(name, hash, &bytes))
            for (size_t i = 0; i < catalog->count; i++)
                if (!strcmp(catalog->entries[i].reference, name)) catalog->entries[i].referenced = 1;
        p += n ? n : 1;
    }
}

static int store_json_refs(store_catalog_t* catalog, const char* text) {
    jp_t check = {text}; if (!jp_skip(&check)) return -1; jp_ws(&check); if (*check.p) return -1;
    size_t capacity = strlen(text) + 1; char* decoded = malloc(capacity);
    if (!decoded) return -1;
    jp_t cursor = {text}; int failed = 0;
    while (*cursor.p) {
        if (*cursor.p == '"') {
            if (jp_string(&cursor, decoded, capacity) < 0) { failed = 1; break; }
            store_mark_refs(catalog, decoded);
        } else cursor.p++;
    }
    free(decoded); return failed ? -1 : 0;
}

static int store_json_field(const char* text, const char* key, char* value, size_t capacity) {
    jp_t j = {text}; jp_ws(&j); if (*j.p++ != '{') return -1;
    int found = 0;
    for (;;) {
        jp_ws(&j); if (*j.p == '}') return found ? 0 : -1;
        char name[128]; if (jp_string(&j, name, sizeof(name)) < 0) return -1;
        jp_ws(&j); if (*j.p++ != ':') return -1; jp_ws(&j);
        if (!strcmp(name, key)) { if (found++ || jp_string(&j, value, capacity) < 0) return -1; }
        else if (!jp_skip(&j)) return -1;
        jp_ws(&j); if (*j.p == ',') j.p++; else if (*j.p != '}') return -1;
    }
}

#define STORE_MAINT_MAX JEF_MAINTENANCE_RESPONSE_MAX
/* Logical peak growth for one new transaction: request, three bounded DSSE
 * envelopes, terminal result (staging renamed in place), optional release marker.
 * This headroom is checked under the exclusive lease; it allocates no disk blocks. */
#define STORE_JOURNAL_HEADROOM ((uint64_t)SRV_AGENT_REQ_MAX + 3u * 8192u + STORE_MAINT_MAX + 65u)
#define STORE_JOURNAL_NEW_FILES 6u

static int maintenance_journal_usage(uint64_t* used, unsigned* entries) {
    *used = 0; *entries = 0;
    int fd = srv_store_dir("agent_maintenance"); if (fd < 0) return errno == ENOENT ? 0 : -1;
    struct stat before, after;
    if (fstat(fd, &before)) { close(fd); return -1; }
    DIR* directory = fdopendir(fd); if (!directory) { close(fd); return -1; }
    int failed = 0;
    for (;;) {
        errno = 0; struct dirent* item = readdir(directory);
        if (!item) { if (errno) failed = 1; break; }
        if (!strcmp(item->d_name, ".") || !strcmp(item->d_name, "..")) continue;
        struct stat st;
        if (*entries == SRV_STORE_MAX_FILES) { failed = 1; break; }
        (*entries)++;
        if (fstatat(fd, item->d_name, &st, AT_SYMLINK_NOFOLLOW) || !S_ISREG(st.st_mode) ||
            st.st_uid != geteuid() || (st.st_mode & 0077) || st.st_nlink != 1 || st.st_size < 0 ||
            (uint64_t)st.st_size > UINT64_MAX - *used) { failed = 1; break; }
        *used += (uint64_t)st.st_size;
    }
    if (fstat(fd, &after) || before.st_dev != after.st_dev || before.st_ino != after.st_ino ||
        before.st_mtime != after.st_mtime || before.st_ctime != after.st_ctime) failed = 1;
    closedir(directory); return failed ? -1 : 0;
}

static const char* maintenance_journal_space(uint64_t* available) {
    int fd = srv_store_dir("agent_maintenance");
    if (fd < 0 && errno == ENOENT) fd = srv_store_dir(".");
    if (fd < 0) return "journal_space_unknown";
    struct statvfs vfs; int failed = fstatvfs(fd, &vfs); close(fd);
    if (failed || !vfs.f_frsize || vfs.f_bavail == (fsblkcnt_t)-1 || vfs.f_bavail > UINT64_MAX / vfs.f_frsize) return "journal_space_unknown";
    *available = (uint64_t)vfs.f_bavail * vfs.f_frsize;
    if ((vfs.f_flag & ST_RDONLY) || *available < STORE_JOURNAL_HEADROOM ||
        (vfs.f_favail != (fsfilcnt_t)-1 && vfs.f_favail < STORE_JOURNAL_NEW_FILES + 2u)) return "journal_space_insufficient";
    return NULL;
}

static const char* maintenance_journal_admit(uint64_t limit, uint64_t used, unsigned entries, int measured,
                                           uint64_t* available, int* space_known) {
    *space_known = 0;
    if (!limit) return NULL;
    if (!measured) return "journal_measurement_incomplete";
    if (used > limit || STORE_JOURNAL_HEADROOM > limit - used) return "journal_quota_exceeded";
    if (entries > SRV_STORE_MAX_FILES - STORE_JOURNAL_NEW_FILES) return "journal_entry_limit";
    const char* error = maintenance_journal_space(available);
    *space_known = !error || !strcmp(error, "journal_space_insufficient");
    return error;
}
static const char* const maintenance_media = "application/vnd.jash.agent-maintenance-receipt.v1+json";
typedef struct { char* bytes; size_t used, sent; int fd, streaming, channel_failed; } maintenance_buffer_t;
typedef struct { char key[65], request_hash[65], intent_ref[128], intent_hash[65], decision_ref[128], decision_hash[65]; int begun, admitted; } maintenance_tx_t;

static long maintenance_sink(void* context, const char* data, size_t size) {
    maintenance_buffer_t* buffer = context;
    if (size >= STORE_MAINT_MAX - buffer->used) { errno = EFBIG; return -1; }
    memcpy(buffer->bytes + buffer->used, data, size); buffer->used += size; buffer->bytes[buffer->used] = 0;
    if (buffer->streaming) {
        if (srv_write_all(buffer->fd, data, size)) { buffer->channel_failed = 1; return -1; }
        buffer->sent = buffer->used;
    }
    return (long)size;
}

static int maintenance_publish(const char* name, const char* bytes, size_t size, int replace) {
    char directory[896], path[1400], temporary[1400]; static unsigned serial;
    if (srv_private_dir("agent_maintenance", directory, sizeof(directory))) return -1;
    int a = snprintf(path, sizeof(path), "%s/%s", directory, name);
    int b = snprintf(temporary, sizeof(temporary), "%s/.%s.%ld.%u.tmp", directory, name, (long)getpid(), ++serial);
    if (a < 0 || (size_t)a >= sizeof(path) || b < 0 || (size_t)b >= sizeof(temporary)) return -1;
    int fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    struct stat owned; int ok = !fchmod(fd, 0600) && !srv_write_all(fd, bytes, size) && !fsync(fd) && !fstat(fd, &owned);
    if (close(fd)) ok = 0;
    /* Failed publication retains staging for diagnosis; no effect is replayed
     * merely because its terminal journal is absent. */
    if (!ok || !srv_regular_identity_ok(temporary, &owned)) return -1;
    if (replace ? rename(temporary, path) : jash_osif_renameat2(AT_FDCWD, temporary, AT_FDCWD, path, 1u)) return -1;
    return srv_regular_identity_ok(path, &owned) && !srv_fsync_dir(directory) ? 0 : 1;
}

static char* maintenance_read(const char* name, size_t max, size_t* length) {
    int directory = srv_store_dir("agent_maintenance"); if (directory < 0) return NULL;
    int fd = openat(directory, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    int saved = errno; close(directory); if (fd < 0) { errno = saved; return NULL; }
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 0077) || st.st_nlink != 1 ||
        st.st_size <= 0 || (uintmax_t)st.st_size > max) { close(fd); errno = EINVAL; return NULL; }
    char* data = malloc((size_t)st.st_size + 1); size_t used = 0;
    if (!data) { close(fd); return NULL; }
    while (used < (size_t)st.st_size) {
        ssize_t n = read(fd, data + used, (size_t)st.st_size - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        used += (size_t)n;
    }
    char extra; ssize_t n = read(fd, &extra, 1); close(fd);
    if (used != (size_t)st.st_size || n != 0 || memchr(data, 0, used)) { free(data); errno = EINVAL; return NULL; }
    data[used] = 0; *length = used; return data;
}

static void maintenance_key(const char* id, char key[65]) {
    unsigned char hash[32]; jash_blake3(id, strlen(id), hash); jash_blake3_hex(hash, key);
}

/* Cooperating writers publish immutable files under the store lease. The same
 * verifier accepts public results and retained staging; neither path re-signs. */
static int maintenance_verify_result(const char* key, const char* name, const char* response,
                                     const srv_receipt_signer_t* signer) {
    char receipt[256], directory[896], qpath[1400], spath[1400], rpath[1400];
    if (store_json_field(response, "receipt_ref", receipt, sizeof(receipt)) || strchr(receipt, '/') || strstr(receipt, "..")) return 0;
    int n = snprintf(directory, sizeof(directory), "%s/.jash/agent_maintenance", srv_home());
    if (n < 0 || (size_t)n >= sizeof(directory)) return 0;
    snprintf(qpath, sizeof(qpath), "%s/%s.intent", directory, key);
    snprintf(spath, sizeof(spath), "%s/%s", directory, name);
    snprintf(rpath, sizeof(rpath), "%s/%s", directory, receipt);
    return jef_verify_maintenance_receipt(rpath, spath, qpath, signer->pubkey_hex) == 0;
}

/* 0 absent, 1 verified terminal, 2 indeterminate/corrupt, 3 ID conflict. */
static int maintenance_lookup(const char* key, const char* expected_request,
                              const srv_receipt_signer_t* signer, char** reply, char** saved_request) {
    *reply = NULL; if (saved_request) *saved_request = NULL;
    char name[96]; size_t size;
    snprintf(name, sizeof(name), "%s.intent", key);
    char* request = maintenance_read(name, SRV_AGENT_REQ_MAX, &size);
    if (!request) {
        if (errno != ENOENT) return 2;
        snprintf(name, sizeof(name), "%s.result", key);
        char* orphan = maintenance_read(name, STORE_MAINT_MAX, &size);
        int absent = !orphan && errno == ENOENT; free(orphan); return absent ? 0 : 2;
    }
    if (expected_request && strcmp(request, expected_request)) { free(request); return 3; }
    char id[48], observed_key[65];
    if (store_json_field(request, "id", id, sizeof(id))) { free(request); return 2; }
    maintenance_key(id, observed_key);
    if (strcmp(key, observed_key)) { free(request); return 2; }
    snprintf(name, sizeof(name), "%s.result", key);
    char* response = maintenance_read(name, STORE_MAINT_MAX, &size);
    if (!response) { free(request); return 2; }
    int ok = maintenance_verify_result(key, name, response, signer);
    if (!ok) { free(request); free(response); return 2; }
    *reply = response; if (saved_request) *saved_request = request; else free(request);
    return 1;
}

static int maintenance_history_complete(const srv_receipt_signer_t* signer) {
    int fd = srv_store_dir("agent_maintenance"); if (fd < 0) return errno == ENOENT;
    DIR* directory = fdopendir(fd); if (!directory) { close(fd); return 0; }
    unsigned entries = 0; uint64_t bytes = 0; int complete = 1;
    for (;;) {
        errno = 0; struct dirent* entry = readdir(directory);
        if (!entry) { if (errno) complete = 0; break; }
        if (entry->d_name[0] == '.') continue;
        if (++entries > SRV_STORE_MAX_FILES) { complete = 0; break; }
        size_t length = strlen(entry->d_name);
        if (length > 5 && !strcmp(entry->d_name + length - 5, ".dsse")) {
            size_t size; char* envelope = maintenance_read(entry->d_name, 8192, &size);
            char encoded[8192], payload[4096], id[48], key[65], intent_name[96];
            long decoded = -1;
            if (envelope && !jef_verify_proof_bytes_pinned(envelope, (unsigned)size, signer->pubkey_hex) &&
                !store_json_field(envelope, "payload", encoded, sizeof(encoded)))
                decoded = srv_b64_decode(encoded, strlen(encoded), (uint8_t*)payload, sizeof(payload) - 1);
            if (decoded < 0) { free(envelope); complete = 0; break; }
            payload[decoded] = 0; bytes += size; free(envelope);
            if (memchr(payload, 0, (size_t)decoded) || store_json_field(payload, "request_id", id, sizeof(id))) { complete = 0; break; }
            maintenance_key(id, key); snprintf(intent_name, sizeof(intent_name), "%s.intent", key);
            struct stat st;
            if (fstatat(fd, intent_name, &st, AT_SYMLINK_NOFOLLOW) || !S_ISREG(st.st_mode) || bytes > SRV_STORE_SCAN_BYTES) { complete = 0; break; }
            continue;
        }
        if (length == 71 && !strcmp(entry->d_name + 64, ".result")) {
            char name[96]; snprintf(name, sizeof(name), "%.*s.intent", 64, entry->d_name); struct stat st;
            if (fstatat(fd, name, &st, AT_SYMLINK_NOFOLLOW) || !S_ISREG(st.st_mode)) { complete = 0; break; }
            continue;
        }
        if (length != 71 || strcmp(entry->d_name + 64, ".intent")) continue;
        char key[65]; memcpy(key, entry->d_name, 64); key[64] = 0;
        char* response = NULL;
        if (maintenance_lookup(key, NULL, signer, &response, NULL) != 1) { complete = 0; break; }
        bytes += strlen(response); free(response);
        if (bytes > SRV_STORE_SCAN_BYTES) { complete = 0; break; }
    }
    closedir(directory); return complete;
}

static int maintenance_staging_kind(const char* name, const char* key, const char* kind) {
    char prefix[80]; snprintf(prefix, sizeof(prefix), ".%s.%s.", key, kind);
    size_t n = strlen(prefix);
    if (strncmp(name, prefix, n)) return 0;
    const char* p = name + n;
    for (int part = 0; part < 2; part++) {
        if (!isdigit((unsigned char)*p)) return 0;
        while (isdigit((unsigned char)*p)) p++;
        if (*p++ != '.') return 0;
    }
    return !strcmp(p, "tmp");
}

static int maintenance_staging_name(const char* name, const char* key) {
    return maintenance_staging_kind(name, key, "result");
}

/* Holds retain the original staging basename, allowing verification/recovery
 * after process death without trusting an unsigned sidecar mapping. */
static int maintenance_gc_name(const char* name, char original[256], char key[65]) {
    int held = !strncmp(name, ".jgc-", 5);
    int n = held ? snprintf(original, 256, ".%s", name + 5) : snprintf(original, 256, "%s", name);
    if (n < 74 || n >= 256 || original[0] != '.') return -1;
    for (int i = 0; i < 64; i++) if (!strchr("0123456789abcdef", original[i + 1])) return -1;
    memcpy(key, original + 1, 64); key[64] = 0;
    return maintenance_staging_name(original, key) ? held : -1;
}

/* Select only fully verified, identical candidates. Scan completion precedes
 * publication; a valid prefix never authorizes ignoring a conflicting suffix. */
static const char* maintenance_candidate_details(const char* key, const char* target,
                                        const srv_receipt_signer_t* signer, char** result, int* existing, char* blocked) {
    *result = NULL; *existing = 0;
    char name[96], id[JASH_AGENT_ID_VALUE_CAP]; size_t length;
    snprintf(name, sizeof(name), "%s.intent", key);
    if (blocked) snprintf(blocked, 256, "%s", name);
    char* request = maintenance_read(name, SRV_AGENT_REQ_MAX, &length);
    if (!request) return errno == ENOENT ? "intent_missing" : "intent_invalid";
    int valid = !store_json_field(request, "id", id, sizeof(id)) && !strcmp(id, target);
    free(request); if (!valid) return "intent_invalid";
    if (maintenance_lookup(key, NULL, signer, result, NULL) == 1) { *existing = 1; if (blocked) blocked[0] = 0; return NULL; }
    if (blocked) blocked[0] = 0;
    int fd = srv_store_dir("agent_maintenance"); if (fd < 0) return "journal_unavailable";
    struct stat st; snprintf(name, sizeof(name), "%s.result", key);
    if (!fstatat(fd, name, &st, AT_SYMLINK_NOFOLLOW) || errno != ENOENT) {
        if (blocked) snprintf(blocked, 256, "%s", name);
        close(fd); return "result_invalid";
    }
    DIR* directory = fdopendir(fd); if (!directory) { close(fd); return "journal_unavailable"; }
    unsigned entries = 0; uint64_t bytes = 0; const char* error = NULL;
    for (;;) {
        errno = 0; struct dirent* entry = readdir(directory);
        if (!entry) { if (errno) { error = "scan_failed"; if (blocked) blocked[0] = 0; } break; }
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        if (++entries > SRV_STORE_MAX_FILES) { error = "scan_limit"; if (blocked) blocked[0] = 0; break; }
        if (!maintenance_staging_name(entry->d_name, key)) continue;
        if (blocked) snprintf(blocked, 256, "%s", entry->d_name);
        if (fstatat(fd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) || !S_ISREG(st.st_mode) || st.st_size <= 0) { error = "candidate_invalid"; break; }
        if ((uintmax_t)st.st_size > STORE_MAINT_MAX || (uint64_t)st.st_size > SRV_STORE_SCAN_BYTES - bytes) { error = "candidate_limit"; break; }
        bytes += (uint64_t)st.st_size;
        char* candidate = maintenance_read(entry->d_name, STORE_MAINT_MAX, &length);
        if (!candidate || !maintenance_verify_result(key, entry->d_name, candidate, signer)) {
            free(candidate); error = "candidate_invalid"; break;
        }
        if (*result && strcmp(*result, candidate)) { free(candidate); error = "candidate_conflict"; break; }
        if (*result) free(candidate); else *result = candidate;
    }
    closedir(directory);
    if (!error && !*result) error = "terminal_unavailable";
    if (error) { free(*result); *result = NULL; }
    else if (blocked) blocked[0] = 0;
    return error;
}

static const char* maintenance_candidate(const char* key, const char* target,
                                        const srv_receipt_signer_t* signer, char** result, int* existing) {
    return maintenance_candidate_details(key, target, signer, result, existing, NULL);
}

typedef struct { char name[256]; const char* role; const char* safety; uint64_t bytes; int measured; } maintenance_file_t;

/* ID-addressed namespace metadata, including staging even when the public intent
 * is missing. This is independent of eligibility; presence is not verification. */
static const char* maintenance_files(const char* key, maintenance_file_t* files, size_t* count, unsigned* scanned) {
    *count = 0; *scanned = 0;
    int fd = srv_store_dir("agent_maintenance"); if (fd < 0) return errno == ENOENT ? NULL : "journal_unavailable";
    DIR* directory = fdopendir(fd); if (!directory) { close(fd); return "journal_unavailable"; }
    char intent[96], result[96];
    snprintf(intent, sizeof(intent), "%s.intent", key); snprintf(result, sizeof(result), "%s.result", key);
    const char* error = NULL;
    for (;;) {
        errno = 0; struct dirent* entry = readdir(directory);
        if (!entry) { if (errno) error = "scan_failed"; break; }
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        if (*scanned == SRV_STORE_MAX_FILES) { error = "scan_limit"; break; }
        (*scanned)++;
        const char* role = !strcmp(entry->d_name, intent) ? "intent" : !strcmp(entry->d_name, result) ? "result" :
            maintenance_staging_name(entry->d_name, key) ? "result_staging" :
            maintenance_staging_kind(entry->d_name, key, "intent") ? "intent_staging" : NULL;
        char original[256], hold_key[65];
        if (!role && maintenance_gc_name(entry->d_name, original, hold_key) == 1 && !strcmp(hold_key, key)) role = "result_gc_hold";
        if (!role) continue;
        maintenance_file_t* item = &files[(*count)++];
        snprintf(item->name, sizeof(item->name), "%s", entry->d_name); item->role = role;
        struct stat st;
        if (fstatat(fd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW)) { item->safety = "unreadable"; continue; }
        item->safety = S_ISREG(st.st_mode) && st.st_uid == geteuid() && !(st.st_mode & 0077) && st.st_nlink == 1 ? "private_regular" : "unsafe";
        if (st.st_size >= 0) { item->measured = 1; item->bytes = (uint64_t)st.st_size; }
    }
    closedir(directory); return error;
}

/* Verified protocol and decision; missing is an observation, not a closure.
 * Only an exclusive resolver may turn a modern preparation into a closed
 * decision, at the same no-replace name used by the admitting runtime. */
static const char* maintenance_admission(const char* key, const srv_receipt_signer_t* signer,
                                        maintenance_tx_t* tx, char** saved, int* protocol) {
    memset(tx, 0, sizeof(*tx)); snprintf(tx->key, sizeof(tx->key), "%s", key);
    *saved = NULL; *protocol = -1;
    char name[96], directory[896], qpath[1400], rpath[1400]; size_t size;
    snprintf(name, sizeof(name), "%s.intent", key);
    char* text = maintenance_read(name, SRV_AGENT_REQ_MAX, &size);
    if (!text) return errno == ENOENT ? "intent_missing" : "intent_invalid";
    char id[JASH_AGENT_ID_VALUE_CAP], observed[65]; unsigned char digest[32];
    if (store_json_field(text, "id", id, sizeof(id))) { free(text); return "intent_invalid"; }
    maintenance_key(id, observed);
    if (strcmp(observed, key)) { free(text); return "intent_invalid"; }
    jash_blake3(text, size, digest); jash_blake3_hex(digest, tx->request_hash);
    snprintf(tx->intent_ref, sizeof(tx->intent_ref), "%s.maintenance-intent.dsse", tx->request_hash);
    int n = snprintf(directory, sizeof(directory), "%s/.jash/agent_maintenance", srv_home());
    if (n < 0 || (size_t)n >= sizeof(directory)) { free(text); return "intent_invalid"; }
    snprintf(qpath, sizeof(qpath), "%s/%s.intent", directory, key);
    snprintf(rpath, sizeof(rpath), "%s/%s", directory, tx->intent_ref);
    char* proof = maintenance_read(tx->intent_ref, 8192, &size);
    if (!proof || jef_verify_maintenance_intent_protocol(rpath, qpath, signer->pubkey_hex, protocol)) {
        free(proof); free(text); return "intent_unverified";
    }
    jash_blake3(proof, size, digest); jash_blake3_hex(digest, tx->intent_hash); free(proof);
    *saved = text;
    if (!*protocol) return "legacy";
    snprintf(tx->decision_ref, sizeof(tx->decision_ref), "%s.maintenance-decision.dsse", tx->request_hash);
    proof = maintenance_read(tx->decision_ref, 8192, &size);
    if (!proof) return errno == ENOENT ? "decision_missing" : "decision_invalid";
    snprintf(rpath, sizeof(rpath), "%s/%s", directory, tx->decision_ref);
    if (jef_verify_maintenance_decision(rpath, qpath, signer->pubkey_hex, &tx->admitted)) { free(proof); return "decision_invalid"; }
    jash_blake3(proof, size, digest); jash_blake3_hex(digest, tx->decision_hash); free(proof);
    tx->begun = 1;
    return tx->admitted ? "admitted" : "closed";
}

typedef struct {
    char name[256], key[67], target[JASH_AGENT_ID_VALUE_CAP];
    struct stat st;
    char* data;
    int safe, signed_id;
} maintenance_history_file_t;

typedef struct {
    char key[67], target[JASH_AGENT_ID_VALUE_CAP], example[256];
    uint64_t bytes;
    unsigned files, intents, results, staging, receipts;
    int signed_id;
    const char* state;
    const char* reason;
    const char* admission;
    const char* verification;
} maintenance_history_row_t;

typedef struct {
    int fd;
    struct stat directory;
    maintenance_history_file_t* files;
    maintenance_history_row_t* rows;
    size_t count, rows_count;
    uint64_t bytes;
    char snapshot[65];
} maintenance_history_t;

static int maintenance_hex64(const char* text) {
    for (int i = 0; i < 64; i++) if (!text[i] || !strchr("0123456789abcdef", text[i])) return 0;
    return 1;
}

static int maintenance_cursor(const char* cursor, char snapshot[65], size_t* offset) {
    if (strlen(cursor) < 69 || strncmp(cursor, "m1:", 3) || !maintenance_hex64(cursor + 3) || cursor[67] != ':') return -1;
    const char* p = cursor + 68;
    if (!isdigit((unsigned char)*p) || (*p == '0' && p[1])) return -1;
    size_t value = 0;
    for (; *p; p++) {
        if (!isdigit((unsigned char)*p) || value > SRV_STORE_MAX_FILES / 10) return -1;
        value = value * 10 + (unsigned)(*p - '0');
        if (value > SRV_STORE_MAX_FILES) return -1;
    }
    memcpy(snapshot, cursor + 3, 64); snapshot[64] = 0; *offset = value; return 0;
}

static void maintenance_hash_u64(struct blake3* hash, uint64_t value) {
    unsigned char bytes[8]; for (int i = 7; i >= 0; i--) { bytes[i] = (unsigned char)value; value >>= 8; }
    blake3_update(hash, bytes, sizeof(bytes));
}

static void maintenance_hash_stat(struct blake3* hash, const struct stat* st) {
    const uint64_t fields[] = {st->st_dev, st->st_ino, st->st_mode, st->st_uid, st->st_gid, st->st_nlink,
        (uint64_t)st->st_size, (uint64_t)st->st_mtime, (uint64_t)st->st_ctime,
#ifdef __APPLE__
        st->st_mtimespec.tv_nsec, st->st_ctimespec.tv_nsec
#else
        st->st_mtim.tv_nsec, st->st_ctim.tv_nsec
#endif
    };
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) maintenance_hash_u64(hash, fields[i]);
}

static int maintenance_same_stat(const struct stat* a, const struct stat* b) {
    struct blake3 one, two; unsigned char x[32], y[32]; blake3_init(&one); blake3_init(&two);
    maintenance_hash_stat(&one, a); maintenance_hash_stat(&two, b);
    blake3_final(&one, x, sizeof(x)); blake3_final(&two, y, sizeof(y)); return !memcmp(x, y, sizeof(x));
}

static int maintenance_file_order(const void* a, const void* b) {
    return strcmp(((const maintenance_history_file_t*)a)->name, ((const maintenance_history_file_t*)b)->name);
}

static int maintenance_row_order(const void* a, const void* b) {
    return strcmp(((const maintenance_history_row_t*)a)->key, ((const maintenance_history_row_t*)b)->key);
}

static void maintenance_history_free(maintenance_history_t* history) {
    for (size_t i = 0; i < history->count; i++) free(history->files[i].data);
    free(history->files); free(history->rows); if (history->fd >= 0) close(history->fd);
}

/* Full bounded content inventory. A cursor is a local observation fingerprint,
 * never an authorization token. No inode is written or snapshot persisted. */
static const char* maintenance_history_stable(maintenance_history_t* history) {
    int fd = srv_store_dir("agent_maintenance");
    if (history->fd < 0) { if (fd >= 0) close(fd); return fd < 0 && errno == ENOENT ? NULL : "journal_changed"; }
    struct stat st;
    int same = fd >= 0 && !fstat(fd, &st) && maintenance_same_stat(&st, &history->directory);
    if (fd >= 0) close(fd);
    if (!same) return "journal_changed";
    for (size_t i = 0; i < history->count; i++)
        if (fstatat(history->fd, history->files[i].name, &st, AT_SYMLINK_NOFOLLOW) || !maintenance_same_stat(&st, &history->files[i].st))
            return "journal_changed";
    return NULL;
}

static const char* maintenance_history_scan(maintenance_history_t* history, const srv_receipt_signer_t* signer) {
    memset(history, 0, sizeof(*history)); history->fd = -1;
    history->files = calloc(SRV_STORE_MAX_FILES, sizeof(*history->files));
    history->rows = calloc(SRV_STORE_MAX_FILES, sizeof(*history->rows));
    if (!history->files || !history->rows) return "out_of_memory";
    history->fd = srv_store_dir("agent_maintenance");
    if (history->fd < 0 && errno != ENOENT) return "journal_unavailable";
    if (history->fd >= 0) {
        if (fstat(history->fd, &history->directory)) return "journal_unavailable";
        int copy = openat(history->fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        DIR* directory = copy >= 0 ? fdopendir(copy) : NULL;
        if (!directory) { if (copy >= 0) close(copy); return "journal_unavailable"; }
        const char* error = NULL;
        for (;;) {
            errno = 0; struct dirent* entry = readdir(directory);
            if (!entry) { if (errno) error = "scan_failed"; break; }
            if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
            if (history->count == SRV_STORE_MAX_FILES) { error = "scan_limit"; break; }
            maintenance_history_file_t* file = &history->files[history->count++];
            snprintf(file->name, sizeof(file->name), "%s", entry->d_name);
            if (fstatat(history->fd, file->name, &file->st, AT_SYMLINK_NOFOLLOW)) { error = "journal_changed"; break; }
        }
        closedir(directory); if (error) return error;
    }
    qsort(history->files, history->count, sizeof(*history->files), maintenance_file_order);
    struct blake3 hash; unsigned char digest[32]; blake3_init(&hash);
    const char domain[] = "jash.maintenance-history.snapshot.v1";
    blake3_update(&hash, domain, sizeof(domain)); blake3_update(&hash, signer->pubkey_hex, strlen(signer->pubkey_hex));
    maintenance_hash_u64(&hash, strlen(srv_home())); blake3_update(&hash, srv_home(), strlen(srv_home()));
    maintenance_hash_u64(&hash, history->fd >= 0); if (history->fd >= 0) maintenance_hash_stat(&hash, &history->directory);
    for (size_t i = 0; i < history->count; i++) {
        maintenance_history_file_t* file = &history->files[i];
        file->safe = S_ISREG(file->st.st_mode) && file->st.st_uid == geteuid() && !(file->st.st_mode & 0077) && file->st.st_nlink == 1 && file->st.st_size >= 0;
        maintenance_hash_u64(&hash, strlen(file->name)); blake3_update(&hash, file->name, strlen(file->name));
        maintenance_hash_stat(&hash, &file->st); maintenance_hash_u64(&hash, file->safe);
        if (file->safe) {
            size_t size = (size_t)file->st.st_size;
            if ((uintmax_t)file->st.st_size > STORE_MAINT_MAX || (uint64_t)size > SRV_STORE_SCAN_BYTES - history->bytes) return "read_limit";
            int fd = openat(history->fd, file->name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC); struct stat st;
            if (fd < 0) return "entry_unreadable";
            if (fstat(fd, &st) || !maintenance_same_stat(&st, &file->st)) { close(fd); return "journal_changed"; }
            file->data = malloc(size + 1); if (!file->data) { close(fd); return "out_of_memory"; }
            size_t used = 0;
            while (used < size) {
                ssize_t n = read(fd, file->data + used, size - used);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) break;
                used += (size_t)n;
            }
            char extra; ssize_t end = read(fd, &extra, 1);
            int stable = !fstat(fd, &st) && maintenance_same_stat(&st, &file->st); close(fd);
            if (!stable || used != size || end != 0) return "journal_changed";
            file->data[size] = 0; history->bytes += size;
            blake3_update(&hash, file->data, size);
        }
        char key[65] = {0}; size_t length = strlen(file->name);
        if (length == 71 && maintenance_hex64(file->name) && (!strcmp(file->name + 64, ".intent") || !strcmp(file->name + 64, ".result"))) {
            memcpy(key, file->name, 64);
        } else if (length > 73 && file->name[0] == '.' && maintenance_hex64(file->name + 1)) {
            memcpy(key, file->name + 1, 64);
            if (!maintenance_staging_kind(file->name, key, "intent") && !maintenance_staging_name(file->name, key)) key[0] = 0;
        }
        if (!key[0]) {
            char original[256], hold_key[65];
            if (maintenance_gc_name(file->name, original, hold_key) == 1) snprintf(key, sizeof(key), "%s", hold_key);
        }
        int text = file->data && !memchr(file->data, 0, (size_t)file->st.st_size);
        if (key[0] && text) {
            char id[JASH_AGENT_ID_VALUE_CAP], observed[65];
            if (!store_json_field(file->data, "id", id, sizeof(id)) && id[0]) {
                maintenance_key(id, observed);
                if (!strcmp(observed, key)) snprintf(file->target, sizeof(file->target), "%s", id);
            }
        }
        if (!key[0] && text && length > 5 && !strcmp(file->name + length - 5, ".dsse") && file->st.st_size <= 8192) {
            char type[128], encoded[8192], payload[4096], schema[96], id[JASH_AGENT_ID_VALUE_CAP], phase[32];
            if (!jef_verify_proof_bytes_pinned(file->data, (unsigned)file->st.st_size, signer->pubkey_hex) &&
                !store_json_field(file->data, "payloadType", type, sizeof(type)) && !strcmp(type, maintenance_media) &&
                !store_json_field(file->data, "payload", encoded, sizeof(encoded))) {
                long decoded = srv_b64_decode(encoded, strlen(encoded), (uint8_t*)payload, sizeof(payload) - 1);
                if (decoded >= 0) {
                    payload[decoded] = 0;
                    if (!memchr(payload, 0, (size_t)decoded) && !store_json_field(payload, "schema", schema, sizeof(schema)) &&
                        !strcmp(schema, "jash.agent-maintenance-receipt.v1") && !store_json_field(payload, "phase", phase, sizeof(phase)) &&
                        (!strcmp(phase, "intent") || !strcmp(phase, "terminal") || !strcmp(phase, "decision")) &&
                        !store_json_field(payload, "request_id", id, sizeof(id)) && id[0]) {
                        maintenance_key(id, key); snprintf(file->target, sizeof(file->target), "%s", id); file->signed_id = 1;
                    }
                }
            }
        }
        if (key[0]) snprintf(file->key, sizeof(file->key), "t:%s", key);
        else { maintenance_key(file->name, key); snprintf(file->key, sizeof(file->key), "u:%s", key); }
        size_t row = 0;
        while (row < history->rows_count && strcmp(history->rows[row].key, file->key)) row++;
        maintenance_history_row_t* item = &history->rows[row];
        if (row == history->rows_count) {
            history->rows_count++; snprintf(item->key, sizeof(item->key), "%s", file->key);
            snprintf(item->example, sizeof(item->example), "%s", file->name);
        }
        item->files++;
        if (file->st.st_size >= 0) {
            uint64_t size = (uint64_t)file->st.st_size;
            item->bytes = size > UINT64_MAX - item->bytes ? UINT64_MAX : item->bytes + size;
        }
        if (length == 71 && !strcmp(file->name + 64, ".intent")) item->intents++;
        if (length == 71 && !strcmp(file->name + 64, ".result")) item->results++;
        if (file->name[0] == '.' && key[0] && file->key[0] == 't') item->staging++;
        if (file->signed_id) item->receipts++;
        if (file->target[0] && (!item->target[0] || file->signed_id)) snprintf(item->target, sizeof(item->target), "%s", file->target);
        item->signed_id |= file->signed_id;
    }
    qsort(history->rows, history->rows_count, sizeof(*history->rows), maintenance_row_order);
    const char* error = maintenance_history_stable(history); if (error) return error;
    blake3_final(&hash, digest, sizeof(digest)); jash_blake3_hex(digest, history->snapshot); return NULL;
}

static void srv_maintenance_history(int respfd, const srv_job_req_t* request, const srv_receipt_signer_t* signer) {
    size_t offset = 0; char expected[65] = {0}; const char* error = NULL;
    if ((request->seen & SRV_JOB_SEEN_CURSOR) && maintenance_cursor(request->cursor, expected, &offset)) {
        srv_job_error(respfd, request, "invalid cursor"); return;
    }
    maintenance_history_t history; error = maintenance_history_scan(&history, signer);
    if (!error && expected[0] && strcmp(expected, history.snapshot)) error = "cursor_stale";
    if (!error && offset > history.rows_count) error = "cursor_range";
    size_t end = !error ? offset + (size_t)request->limit : 0;
    if (end > history.rows_count) end = history.rows_count;
    uint64_t budget = SRV_STORE_SCAN_BYTES;
    for (size_t i = offset; !error && i < end; i++) {
        maintenance_history_row_t* row = &history.rows[i];
        row->state = "unresolved"; row->reason = "intent_missing"; row->admission = "unknown"; row->verification = "not_checked";
        if (row->key[0] == 'u') { row->state = "unattributed"; row->reason = "unattributed_file"; row->verification = "not_applicable"; continue; }
        /* Conservative charge bounds repeated request/result/receipt reads by
         * these helpers. Exhaustion preserves the row as explicitly unchecked. */
        if (row->bytes > (SRV_STORE_SCAN_BYTES - 131072) / 8 || row->bytes * 8 + 131072 > budget) { row->reason = "verification_limit"; continue; }
        budget -= row->bytes * 8 + 131072;
        char* result = NULL; row->verification = "checked";
        int found = maintenance_lookup(row->key + 2, NULL, signer, &result, NULL);
        if (found == 1) { row->state = "committed"; row->reason = NULL; }
        else if (found == 2) row->reason = "maintenance_indeterminate";
        free(result);
        maintenance_tx_t admission; char* text = NULL; int protocol;
        row->admission = maintenance_admission(row->key + 2, signer, &admission, &text, &protocol); free(text);
    }
    if (!error) error = maintenance_history_stable(&history);
    if (error) end = offset;
    char cursor[96]; cursor[0] = 0;
    if (!error && end < history.rows_count) snprintf(cursor, sizeof(cursor), "m1:%s:%zu", history.snapshot, end);
    jash_json_writer_t out; jash_jsonw_init(&out, jash_jsonw_fd_sink, &respfd);
    jash_jsonw_raw(&out, "{\"schema\":\"jash.agent-maintenance-history.v1\",\"id\":"); jash_jsonw_string(&out, request->id_value);
    jash_jsonw_raw(&out, ",\"op\":\"store.history\",\"authority\":\"observation-only\",\"snapshot\":");
    if (!error) jash_jsonw_string(&out, history.snapshot); else jash_jsonw_raw(&out, "null");
    jash_jsonw_raw(&out, ",\"next_cursor\":"); if (cursor[0]) jash_jsonw_string(&out, cursor); else jash_jsonw_raw(&out, "null");
    jash_jsonw_raw(&out, ",\"offset\":"); jash_jsonw_safe_u64(&out, offset);
    jash_jsonw_raw(&out, ",\"limit\":"); jash_jsonw_safe_u64(&out, (uint64_t)request->limit);
    jash_jsonw_raw(&out, ",\"total_records\":"); if (!error) jash_jsonw_safe_u64(&out, history.rows_count); else jash_jsonw_raw(&out, "null");
    jash_jsonw_raw(&out, ",\"entries_scanned\":"); jash_jsonw_safe_u64(&out, history.count);
    jash_jsonw_raw(&out, ",\"bytes_read\":"); jash_jsonw_safe_u64(&out, history.bytes);
    jash_jsonw_raw(&out, ",\"scan_complete\":"); jash_jsonw_raw(&out, error ? "false" : "true");
    jash_jsonw_raw(&out, ",\"items\":[");
    for (size_t i = offset; i < end && !out.error; i++) {
        maintenance_history_row_t* row = &history.rows[i]; if (i != offset) jash_jsonw_raw(&out, ",");
        jash_jsonw_raw(&out, "{\"record_key\":"); jash_jsonw_string(&out, row->key);
        jash_jsonw_raw(&out, ",\"target_id\":"); if (row->target[0]) jash_jsonw_string(&out, row->target); else jash_jsonw_raw(&out, "null");
        jash_jsonw_raw(&out, ",\"id_source\":"); jash_jsonw_string(&out, !row->target[0] ? "unknown" : row->signed_id ? "signed_receipt" : "unverified_document");
        jash_jsonw_raw(&out, ",\"example_ref\":"); jash_jsonw_string_or_bytes(&out, row->example);
        jash_jsonw_raw(&out, ",\"files\":"); jash_jsonw_safe_u64(&out, row->files);
        jash_jsonw_raw(&out, ",\"intent_files\":"); jash_jsonw_safe_u64(&out, row->intents);
        jash_jsonw_raw(&out, ",\"result_files\":"); jash_jsonw_safe_u64(&out, row->results);
        jash_jsonw_raw(&out, ",\"staging_files\":"); jash_jsonw_safe_u64(&out, row->staging);
        jash_jsonw_raw(&out, ",\"signed_receipts\":"); jash_jsonw_safe_u64(&out, row->receipts);
        jash_jsonw_raw(&out, ",\"state\":"); jash_jsonw_string(&out, row->state);
        jash_jsonw_raw(&out, ",\"reason\":"); if (row->reason) jash_jsonw_string(&out, row->reason); else jash_jsonw_raw(&out, "null");
        jash_jsonw_raw(&out, ",\"effect_admission\":"); jash_jsonw_string(&out, row->admission);
        jash_jsonw_raw(&out, ",\"verification\":"); jash_jsonw_string(&out, row->verification);
        jash_jsonw_raw(&out, "}");
    }
    jash_jsonw_raw(&out, "],\"ok\":"); jash_jsonw_raw(&out, error ? "false" : "true");
    jash_jsonw_raw(&out, ",\"error\":"); if (error) jash_jsonw_string(&out, error); else jash_jsonw_raw(&out, "null");
    jash_jsonw_raw(&out, "}\n"); (void)jash_jsonw_finish(&out); maintenance_history_free(&history);
}

static int maintenance_sync_result(const char*);

static int maintenance_decide(maintenance_tx_t* tx, const srv_job_req_t* request, const srv_receipt_signer_t* signer, int admitted) {
    char payload[1536];
    snprintf(tx->decision_ref, sizeof(tx->decision_ref), "%s.maintenance-decision.dsse", request->request_digest);
    int n = snprintf(payload, sizeof(payload), "{\"schema\":\"jash.agent-maintenance-receipt.v1\",\"phase\":\"decision\",\"request_id\":%s,\"operation\":\"%s\",\"request_blake3\":\"%s\",\"intent_ref\":\"%s\",\"intent_blake3\":\"%s\",\"decision\":\"%s\"}",
        request->id, request->op, request->request_digest, tx->intent_ref, tx->intent_hash, admitted ? "admitted" : "closed");
    if (n < 0 || (size_t)n >= sizeof(payload) || srv_typed_receipt_publish(signer, payload, tx->decision_ref, tx->decision_hash,
        "agent_maintenance", maintenance_media) || maintenance_sync_result(tx->decision_ref)) return -1;
    tx->begun = 1; tx->admitted = admitted; return 0;
}

static int maintenance_commit(maintenance_tx_t*, const srv_job_req_t*, maintenance_buffer_t*, const srv_receipt_signer_t*);
static void srv_job_req_free(srv_job_req_t*);

/* Publish a terminal refusal for the original request, never dispatch its effect.
 * Existing invalid/partial terminal staging is refused by candidate selection
 * before entering this path. Decision retries use identical signed bytes. */
static int maintenance_close(maintenance_tx_t* tx, const char* text, const srv_receipt_signer_t* signer) {
    srv_job_req_t* original = calloc(1, sizeof(*original));
    if (!original) return -1;
    int failed = 1;
    if (srv_job_parse(text, original)) goto done;
    snprintf(original->request_digest, sizeof(original->request_digest), "%s", tx->request_hash);
    if (maintenance_decide(tx, original, signer, 0)) goto done;
    maintenance_buffer_t buffer = {0}; buffer.bytes = malloc(STORE_MAINT_MAX);
    if (!buffer.bytes) goto done;
    buffer.bytes[0] = 0;
    jash_json_writer_t out; jash_jsonw_init(&out, maintenance_sink, &buffer);
    jash_jsonw_raw(&out, "{\"schema\":\"jash.agent-store.v1\",\"id\":"); jash_jsonw_string(&out, original->id_value);
    jash_jsonw_raw(&out, ",\"op\":"); jash_jsonw_string(&out, original->op);
    jash_jsonw_raw(&out, ",\"quota_scope\":\"cas-and-job-archives-including-staging\",\"quota_bytes\":null,\"used_bytes\":null,\"dry_run\":");
    jash_jsonw_raw(&out, original->dry_run ? "true" : "false");
    jash_jsonw_raw(&out, ",\"applied\":false,\"root_retention\":\"explicit-release\",\"scan_complete\":false,\"root_error\":null,\"coordination\":\"exclusive\",\"entries_scanned\":0,\"reclaimed_files\":0,\"reclaimed_bytes\":0,\"planned_files\":0,\"reconciled_files\":0,\"items\":[],\"effect_admission\":\"closed\",\"transaction\":\"committed\",\"maintenance_complete\":false,\"ok\":false,\"error\":\"effects_not_admitted\"}\n");
    if (!jash_jsonw_finish(&out)) failed = maintenance_commit(tx, original, &buffer, signer);
    free(buffer.bytes);
done:
    srv_job_req_free(original); return failed;
}

static void srv_maintenance_inspect(int respfd, const srv_job_req_t* request, const srv_receipt_signer_t* signer) {
    maintenance_file_t* files = calloc(SRV_STORE_MAX_FILES, sizeof(*files));
    if (!files) { srv_job_error(respfd, request, "out of memory"); return; }
    char key[65], intent[96], public[96]; maintenance_key(request->target_id, key);
    snprintf(intent, sizeof(intent), "%s.intent", key); snprintf(public, sizeof(public), "%s.result", key);
    size_t count, length; unsigned scanned;
    const char* scan_error = maintenance_files(key, files, &count, &scanned);
    char* text = maintenance_read(intent, SRV_AGENT_REQ_MAX, &length);
    const char* intent_status = text ? "readable" : errno == ENOENT ? "missing" : "unreadable";
    const char* proof_status = "unknown"; char intent_ref[128] = {0}, operation[32] = {0};
    if (text) {
        unsigned char digest[32]; char hash[65], directory[896], qpath[1400], rpath[1400];
        jash_blake3(text, length, digest); jash_blake3_hex(digest, hash);
        snprintf(intent_ref, sizeof(intent_ref), "%s.maintenance-intent.dsse", hash);
        size_t size; char* proof = maintenance_read(intent_ref, 8192, &size);
        proof_status = proof ? "invalid" : errno == ENOENT ? "missing" : "unreadable";
        int n = snprintf(directory, sizeof(directory), "%s/.jash/agent_maintenance", srv_home());
        char id[JASH_AGENT_ID_VALUE_CAP];
        if (proof && n >= 0 && (size_t)n < sizeof(directory) &&
            !store_json_field(text, "id", id, sizeof(id)) && !strcmp(id, request->target_id)) {
            snprintf(qpath, sizeof(qpath), "%s/%s", directory, intent);
            snprintf(rpath, sizeof(rpath), "%s/%s", directory, intent_ref);
            if (!jef_verify_maintenance_intent(rpath, qpath, signer->pubkey_hex)) {
                proof_status = "verified"; (void)store_json_field(text, "op", operation, sizeof(operation));
            }
        }
        free(proof); free(text);
    }
    text = maintenance_read(public, STORE_MAINT_MAX, &length);
    const char* result_status = text ? "unverified" : errno == ENOENT ? "missing" : "unreadable"; free(text);
    char* candidate = NULL; int existing = 0; char blocked[256] = {0};
    const char* reason = scan_error ? scan_error : maintenance_candidate_details(key, request->target_id, signer, &candidate, &existing, blocked);
    const char* state = reason ? "unresolved" : existing ? "committed" : "recoverable";
    maintenance_tx_t admission; char* saved = NULL; int protocol;
    const char* admission_state = maintenance_admission(key, signer, &admission, &saved, &protocol);
    int closable = reason && !strcmp(reason, "terminal_unavailable") && protocol == 1 &&
        (!strcmp(admission_state, "decision_missing") || !strcmp(admission_state, "closed"));
    if (closable) { reason = NULL; state = "closable"; blocked[0] = 0; }
    free(saved);
    char receipt[256] = {0};
    if (!reason && candidate) {
        (void)store_json_field(candidate, "receipt_ref", receipt, sizeof(receipt));
        if (existing) result_status = "verified";
    } else if (reason && !strcmp(reason, "result_invalid") && !strcmp(result_status, "unverified")) result_status = "invalid";
    int complete = maintenance_history_complete(signer);
    jash_json_writer_t out; jash_jsonw_init(&out, jash_jsonw_fd_sink, &respfd);
    jash_jsonw_raw(&out, "{\"schema\":\"jash.agent-maintenance-inspection.v1\",\"id\":"); jash_jsonw_string(&out, request->id_value);
    jash_jsonw_raw(&out, ",\"op\":\"store.inspect\",\"target_id\":"); jash_jsonw_string(&out, request->target_id);
    jash_jsonw_raw(&out, ",\"observation\":\"live\",\"state\":"); jash_jsonw_string(&out, state);
    jash_jsonw_raw(&out, ",\"reason\":"); if (reason) jash_jsonw_string(&out, reason); else jash_jsonw_raw(&out, "null");
    jash_jsonw_raw(&out, ",\"blocked_ref\":"); if (blocked[0]) jash_jsonw_string(&out, blocked); else jash_jsonw_raw(&out, "null");
    jash_jsonw_raw(&out, ",\"operation\":"); if (operation[0]) jash_jsonw_string(&out, operation); else jash_jsonw_raw(&out, "null");
    jash_jsonw_raw(&out, ",\"effect_protocol\":"); jash_jsonw_string(&out, protocol == 1 ? "admission-v1" : protocol == 0 ? "legacy" : "unknown");
    jash_jsonw_raw(&out, ",\"effect_admission\":"); jash_jsonw_string(&out, admission_state);
    jash_jsonw_raw(&out, ",\"decision_ref\":"); if (admission.decision_ref[0]) jash_jsonw_string(&out, admission.decision_ref); else jash_jsonw_raw(&out, "null");
    jash_jsonw_raw(&out, ",\"resolution_eligible\":"); jash_jsonw_raw(&out, reason ? "false" : "true");
    jash_jsonw_raw(&out, ",\"eligibility_scope\":\"evidence-only\",\"maintenance_complete\":"); jash_jsonw_raw(&out, complete ? "true" : "false");
    jash_jsonw_raw(&out, ",\"intent\":{\"ref\":"); jash_jsonw_string(&out, intent);
    jash_jsonw_raw(&out, ",\"status\":"); jash_jsonw_string(&out, intent_status);
    jash_jsonw_raw(&out, ",\"receipt_ref\":"); if (intent_ref[0]) jash_jsonw_string(&out, intent_ref); else jash_jsonw_raw(&out, "null");
    jash_jsonw_raw(&out, ",\"receipt_status\":"); jash_jsonw_string(&out, proof_status);
    jash_jsonw_raw(&out, "},\"result\":{\"ref\":"); jash_jsonw_string(&out, public);
    jash_jsonw_raw(&out, ",\"status\":"); jash_jsonw_string(&out, result_status);
    jash_jsonw_raw(&out, "},\"terminal_receipt_ref\":"); if (receipt[0]) jash_jsonw_string(&out, receipt); else jash_jsonw_raw(&out, "null");
    jash_jsonw_raw(&out, ",\"receipt_pubkey\":"); jash_jsonw_string(&out, signer->pubkey_hex);
    jash_jsonw_raw(&out, ",\"scan_complete\":"); jash_jsonw_raw(&out, scan_error ? "false" : "true");
    jash_jsonw_raw(&out, ",\"entries_scanned\":"); jash_jsonw_safe_u64(&out, scanned);
    jash_jsonw_raw(&out, ",\"files\":[");
    for (size_t i = 0; i < count && !out.error; i++) {
        if (i) jash_jsonw_raw(&out, ",");
        jash_jsonw_raw(&out, "{\"ref\":"); jash_jsonw_string(&out, files[i].name);
        jash_jsonw_raw(&out, ",\"role\":"); jash_jsonw_string(&out, files[i].role);
        jash_jsonw_raw(&out, ",\"safety\":"); jash_jsonw_string(&out, files[i].safety);
        jash_jsonw_raw(&out, ",\"bytes\":"); if (files[i].measured) jash_jsonw_safe_u64(&out, files[i].bytes); else jash_jsonw_raw(&out, "null");
        jash_jsonw_raw(&out, "}");
    }
    jash_jsonw_raw(&out, "],\"ok\":"); jash_jsonw_raw(&out, scan_error ? "false" : "true");
    jash_jsonw_raw(&out, ",\"error\":"); if (scan_error) jash_jsonw_string(&out, scan_error); else jash_jsonw_raw(&out, "null");
    jash_jsonw_raw(&out, "}\n"); (void)jash_jsonw_finish(&out); free(candidate); free(files);
}

/* A prior directory-sync failure may leave an already-visible result. Explicit
 * resolution synchronizes that publication again before confirming durability. */
static int maintenance_sync_result(const char* name) {
    int directory = srv_store_dir("agent_maintenance"); if (directory < 0) return -1;
    int fd = openat(directory, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    int failed = fd < 0 || fsync(fd);
    if (fd >= 0 && close(fd)) failed = 1;
    if (!failed && fsync(directory)) failed = 1;
    close(directory); return failed ? -1 : 0;
}

static void srv_store_resolve(int respfd, srv_job_table_t* table, const srv_job_req_t* request,
                              const srv_receipt_signer_t* signer) {
    jash_json_writer_t out; jash_jsonw_init(&out, jash_jsonw_fd_sink, &respfd);
    jash_jsonw_raw(&out, "{\"schema\":\"jash.agent-maintenance-resolution.v1\",\"id\":");
    jash_jsonw_string(&out, request->id_value);
    jash_jsonw_raw(&out, ",\"op\":\"store.resolve\",\"target_id\":"); jash_jsonw_string(&out, request->target_id);
    if (jash_jsonw_finish(&out)) return;
    const char* error = NULL; const char* status = "unresolved";
    int locked = 0, existing = 0, published = 0; char* result = NULL;
    char key[65], name[96], receipt[256] = {0}; maintenance_key(request->target_id, key);
    snprintf(name, sizeof(name), "%s.result", key);
    for (int i = 0; i < SRV_JOB_MAX; i++) if (table->jobs[i].used) error = "session_has_jobs";
    char lease_path[1100]; struct stat lease;
    snprintf(lease_path, sizeof(lease_path), "%s/.jash/agent_store/lease", srv_home());
    if (!error && (fstat(srv_store_lease, &lease) || !srv_regular_identity_ok(lease_path, &lease))) error = "lease_identity_changed";
    if (!error && flock(srv_store_lease, LOCK_EX | LOCK_NB)) {
        error = "store_busy";
        if (flock(srv_store_lease, LOCK_SH)) { error = "lease_restore_failed"; srv_store_poisoned = 1; }
    } else if (!error) locked = 1;
    if (!error) error = maintenance_candidate(key, request->target_id, signer, &result, &existing);
    if (error && locked && !strcmp(error, "terminal_unavailable")) {
        maintenance_tx_t admission; char* saved = NULL; int protocol;
        const char* choice = maintenance_admission(key, signer, &admission, &saved, &protocol);
        if (protocol == 1 && (!strcmp(choice, "decision_missing") || !strcmp(choice, "closed"))) {
            jash_jsonw_raw(&out, " ");
            if (jash_jsonw_finish(&out)) error = "delivery_failed";
            else if (srv_shutdown_requested()) error = "shutdown";
            else if (request->dry_run) { status = "closable"; error = NULL; }
            else if (maintenance_close(&admission, saved, signer)) {
                error = "closure_unconfirmed";
                if (maintenance_lookup(key, NULL, signer, &result, NULL) == 1) {
                    published = 1;
                    (void)store_json_field(result, "receipt_ref", receipt, sizeof(receipt));
                }
            }
            else {
                published = 1;
                if (maintenance_lookup(key, NULL, signer, &result, NULL) == 1) {
                    status = "closed"; error = NULL;
                    (void)store_json_field(result, "receipt_ref", receipt, sizeof(receipt));
                } else error = "closure_unconfirmed";
            }
        }
        free(saved);
    }
    if (!error && strcmp(status, "closed") && strcmp(status, "closable")) {
        (void)store_json_field(result, "receipt_ref", receipt, sizeof(receipt));
        jash_jsonw_raw(&out, " ");
        if (jash_jsonw_finish(&out)) error = "delivery_failed";
        else if (srv_shutdown_requested()) error = "shutdown";
        else if (request->dry_run) status = existing ? "already_committed" : "recoverable";
        else if (existing) {
            if (maintenance_sync_result(name)) error = "publication_unconfirmed";
            else status = "already_committed";
        } else {
            int rc = maintenance_publish(name, result, strlen(result), 0);
            published = rc >= 0;
            if (rc) error = "publication_unconfirmed"; else status = "recovered";
        }
    }
    int complete = maintenance_history_complete(signer);
    if (locked && flock(srv_store_lease, LOCK_SH)) { srv_store_poisoned = 1; error = "lease_restore_failed"; }
    jash_jsonw_raw(&out, ",\"evidence_scope\":\"original-maintenance-terminal\",\"status\":"); jash_jsonw_string(&out, status);
    jash_jsonw_raw(&out, ",\"journal_quota_mode\":\"recovery-exempt\"");
    jash_jsonw_raw(&out, ",\"dry_run\":"); jash_jsonw_raw(&out, request->dry_run ? "true" : "false");
    jash_jsonw_raw(&out, ",\"published\":"); jash_jsonw_raw(&out, published ? "true" : "false");
    jash_jsonw_raw(&out, ",\"maintenance_complete\":"); jash_jsonw_raw(&out, complete ? "true" : "false");
    jash_jsonw_raw(&out, ",\"result_ref\":"); if (result) jash_jsonw_string(&out, name); else jash_jsonw_raw(&out, "null");
    jash_jsonw_raw(&out, ",\"receipt_ref\":"); if (receipt[0]) jash_jsonw_string(&out, receipt); else jash_jsonw_raw(&out, "null");
    jash_jsonw_raw(&out, ",\"receipt_pubkey\":"); jash_jsonw_string(&out, signer->pubkey_hex);
    jash_jsonw_raw(&out, ",\"ok\":"); jash_jsonw_raw(&out, error ? "false" : "true");
    jash_jsonw_raw(&out, ",\"error\":"); if (error) jash_jsonw_string(&out, error); else jash_jsonw_raw(&out, "null");
    jash_jsonw_raw(&out, "}\n"); (void)jash_jsonw_finish(&out); free(result);
}

static int maintenance_begin(maintenance_tx_t* tx, const srv_job_req_t* request, const char* text, const srv_receipt_signer_t* signer) {
    char name[96], payload[1024];
    snprintf(name, sizeof(name), "%s.intent", tx->key);
    if (maintenance_publish(name, text, strlen(text), 0)) return -1;
    snprintf(tx->intent_ref, sizeof(tx->intent_ref), "%s.maintenance-intent.dsse", request->request_digest);
    int n = snprintf(payload, sizeof(payload), "{\"schema\":\"jash.agent-maintenance-receipt.v1\",\"phase\":\"intent\",\"effect_protocol\":\"admission-v1\",\"request_id\":%s,\"operation\":\"%s\",\"request_blake3\":\"%s\"}",
                     request->id, request->op, request->request_digest);
    if (n < 0 || (size_t)n >= sizeof(payload) || srv_typed_receipt_publish(signer, payload, tx->intent_ref, tx->intent_hash,
                                                                         "agent_maintenance", maintenance_media)) return -1;
    return maintenance_decide(tx, request, signer, 1);
}

static int maintenance_commit(maintenance_tx_t* tx, const srv_job_req_t* request,
                             maintenance_buffer_t* result, const srv_receipt_signer_t* signer) {
    if (result->used < 2 || strcmp(result->bytes + result->used - 2, "}\n")) return -1;
    unsigned char digest[32]; char result_hash[65], receipt_hash[65], receipt_ref[224], payload[1536];
    jash_blake3(result->bytes, result->used, digest); jash_blake3_hex(digest, result_hash);
    snprintf(receipt_ref, sizeof(receipt_ref), "%s.%s.maintenance-terminal.dsse", request->request_digest, result_hash);
    int n = snprintf(payload, sizeof(payload), "{\"schema\":\"jash.agent-maintenance-receipt.v1\",\"phase\":\"terminal\",\"request_id\":%s,\"operation\":\"%s\",\"request_blake3\":\"%s\",\"response_blake3\":\"%s\",\"intent_ref\":\"%s\",\"intent_blake3\":\"%s\",\"decision_ref\":\"%s\",\"decision_blake3\":\"%s\"}",
        request->id, request->op, request->request_digest, result_hash, tx->intent_ref, tx->intent_hash, tx->decision_ref, tx->decision_hash);
    if (n < 0 || (size_t)n >= sizeof(payload) || srv_typed_receipt_publish(signer, payload, receipt_ref, receipt_hash,
                                                                         "agent_maintenance", maintenance_media)) return -1;
    result->used -= 2;
    n = snprintf(result->bytes + result->used, STORE_MAINT_MAX - result->used,
        ",\"receipt_phase\":\"maintenance\",\"receipt_ref\":\"%s\",\"receipt_blake3\":\"%s\",\"receipt_pubkey\":\"%s\"}\n", receipt_ref, receipt_hash, signer->pubkey_hex);
    if (n < 0 || (size_t)n >= STORE_MAINT_MAX - result->used) return -1;
    result->used += (size_t)n;
    char name[96]; snprintf(name, sizeof(name), "%s.result", tx->key);
    return maintenance_publish(name, result->bytes, result->used, 0);
}

static void maintenance_release_name(const char* namespace, const char* name, char out[96]) {
    struct blake3 hash; unsigned char digest[32]; char hex[65];
    blake3_init(&hash); blake3_update(&hash, namespace, strlen(namespace)); blake3_update(&hash, "\n", 1);
    blake3_update(&hash, name, strlen(name)); blake3_final(&hash, digest, sizeof(digest)); jash_blake3_hex(digest, hex);
    snprintf(out, 96, "%s.release", hex);
}

static int store_json_true(const char* text, const char* key) {
    jp_t j = {text}; jp_ws(&j); if (*j.p++ != '{') return 0;
    for (;;) {
        jp_ws(&j); if (*j.p == '}') return 0;
        char field[128]; if (jp_string(&j, field, sizeof(field)) < 0) return 0;
        jp_ws(&j); if (*j.p++ != ':') return 0; jp_ws(&j);
        if (!strcmp(field, key)) { int value = 0; return !jp_bool(&j, &value) && value; }
        if (!jp_skip(&j)) return 0;
        jp_ws(&j); if (*j.p == ',') j.p++; else if (*j.p != '}') return 0;
    }
}

static int maintenance_root_released(const char* namespace, const char* name, const char* hash,
                                     const srv_receipt_signer_t* signer, uint64_t* budget) {
    char marker_name[96]; maintenance_release_name(namespace, name, marker_name);
    size_t length; char* marker = maintenance_read(marker_name, 65, &length);
    if (!marker) return errno == ENOENT ? 0 : -1;
    if (length != 65 || marker[64] != '\n') { free(marker); return -1; }
    marker[64] = 0;
    for (int i = 0; i < 64; i++) if (!strchr("0123456789abcdef", marker[i])) { free(marker); return -1; }
    char *response = NULL, *request = NULL;
    int status = maintenance_lookup(marker, NULL, signer, &response, &request); free(marker);
    if (status != 1) return -1;
    *budget += strlen(request) + strlen(response);
    char operation[32], ns[32], root[256], digest[65];
    int released = *budget <= SRV_STORE_SCAN_BYTES && !store_json_field(request, "op", operation, sizeof(operation)) &&
        !strcmp(operation, "store.release") && !store_json_field(request, "root_namespace", ns, sizeof(ns)) && !strcmp(ns, namespace) &&
        !store_json_field(request, "root_name", root, sizeof(root)) && !strcmp(root, name) &&
        !store_json_field(request, "root_blake3", digest, sizeof(digest)) && !strcmp(digest, hash) &&
        store_json_true(response, "ok") && store_json_true(response, "applied");
    free(response); free(request); return *budget > SRV_STORE_SCAN_BYTES ? -1 : released;
}

static char* store_load(store_catalog_t* catalog, store_entry_t* entry) {
    if (!S_ISREG(entry->st.st_mode) || entry->st.st_uid != geteuid() || (entry->st.st_mode & 0077) || entry->st.st_nlink != 1 ||
        entry->st.st_size < 0 || entry->st.st_size > SRV_IDEM_MAX || catalog->root_bytes + (uint64_t)entry->st.st_size > SRV_STORE_SCAN_BYTES) return NULL;
    int fd = openat(catalog->fds[entry->directory], entry->name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    struct stat st;
    if (fd < 0) return NULL;
    if (fstat(fd, &st) || st.st_dev != entry->st.st_dev || st.st_ino != entry->st.st_ino || st.st_size != entry->st.st_size) { close(fd); return NULL; }
    size_t size = (size_t)st.st_size, used = 0; char* data = malloc(size + 1);
    if (!data) { close(fd); return NULL; }
    while (used < size) {
        ssize_t n = read(fd, data + used, size - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        used += (size_t)n;
    }
    char extra; ssize_t trailing = read(fd, &extra, 1); close(fd);
    if (used != size || trailing != 0) { free(data); return NULL; }
    data[size] = 0; catalog->root_bytes += size; return data;
}

static void store_catalog_free(store_catalog_t* catalog) {
    for (int i = 0; i < 5; i++) if (catalog->fds[i] >= 0) close(catalog->fds[i]);
    free(catalog->entries);
}

static void store_catalog_scan(store_catalog_t* catalog, const srv_receipt_signer_t* signer) {
    memset(catalog, 0, sizeof(*catalog)); catalog->complete = 1;
    for (int i = 0; i < 5; i++) catalog->fds[i] = -1;
    catalog->entries = calloc(SRV_STORE_MAX_FILES, sizeof(*catalog->entries));
    if (!catalog->entries) { catalog->complete = 0; return; }
    for (int d = 0; d < 5; d++) {
        catalog->fds[d] = srv_store_dir(store_dirs[d]);
        if (catalog->fds[d] < 0) { if (errno != ENOENT) catalog->complete = 0; continue; }
        int copy = openat(catalog->fds[d], ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        DIR* directory = copy >= 0 ? fdopendir(copy) : NULL;
        if (!directory) { if (copy >= 0) close(copy); catalog->complete = 0; continue; }
        for (;;) {
            errno = 0; struct dirent* item = readdir(directory);
            if (!item) { if (errno) catalog->complete = 0; break; }
            if (!strcmp(item->d_name, ".") || !strcmp(item->d_name, "..")) continue;
            if (catalog->count == SRV_STORE_MAX_FILES) { catalog->complete = 0; break; }
            size_t name_length = strnlen(item->d_name, sizeof(item->d_name));
            if (name_length == sizeof(item->d_name) ||
                name_length >= sizeof(catalog->entries[0].name)) {
                catalog->complete = 0; break;
            }
            store_entry_t* entry = &catalog->entries[catalog->count++];
            memcpy(entry->name, item->d_name, name_length + 1);
            entry->directory = d; entry->kind = STORE_OTHER;
            if (fstatat(catalog->fds[d], entry->name, &entry->st, AT_SYMLINK_NOFOLLOW)) { catalog->complete = 0; continue; }
            entry->metadata_valid = 1;
            char hash[65]; uint64_t size;
            int held = !strncmp(entry->name, ".gc-", 4);
            const char* original = entry->name + (held ? 4 : 0);
            if (!d && store_cas_name(original, hash, &size)) {
                size_t reference_length = strlen(original);
                if (reference_length >= sizeof(entry->reference)) { catalog->complete = 0; continue; }
                entry->kind = held ? STORE_HOLD : STORE_CAS;
                memcpy(entry->reference, original, reference_length + 1);
            }
            else if (!d && !strncmp(original, ".cas-", 5)) entry->kind = STORE_STAGE;
            else if (d == 1) entry->kind = STORE_ARCHIVE;
            else if (d == 2) entry->kind = STORE_RECEIPT;
            else if (d == 3) entry->kind = STORE_REQUEST;
            else if (d == 4) entry->kind = STORE_SPOOL;
        }
        closedir(directory);
    }
    for (size_t i = 0; i < catalog->count; i++) {
        if (!catalog->complete) break;
        store_entry_t* entry = &catalog->entries[i];
        if (entry->kind < STORE_ARCHIVE || entry->kind == STORE_OTHER) continue;
        catalog->root_error = entry->name;
        catalog->suppress_refs = 0;
        if (entry->kind != STORE_SPOOL) {
            char* root = store_load(catalog, entry);
            if (!root) { catalog->complete = 0; continue; }
            unsigned char digest[32]; jash_blake3(root, (size_t)entry->st.st_size, digest); free(root);
            jash_blake3_hex(digest, entry->root_hash);
            int released = maintenance_root_released(store_dirs[entry->directory], entry->name, entry->root_hash, signer, &catalog->maintenance_bytes);
            if (released < 0) { catalog->complete = 0; continue; }
            entry->released = released; catalog->suppress_refs = released;
        }
        if (entry->kind == STORE_ARCHIVE || entry->kind == STORE_SPOOL) {
            char job_id[35];
            const char* original = entry->name + (!strncmp(entry->name, ".gc-", 4) ? 4 : 0);
            if (strlen(original) < 35) { catalog->complete = 0; continue; }
            memcpy(job_id, original, 34); job_id[34] = 0;
            const char* suffix = original + 34;
            if ((entry->kind == STORE_ARCHIVE && strcmp(suffix, ".result")) ||
                (entry->kind == STORE_SPOOL && strcmp(suffix, ".stdout") && strcmp(suffix, ".stderr"))) { catalog->complete = 0; continue; }
            char archive[SRV_JOB_ARCHIVE_MAX]; const char *request, *response;
            if (catalog->root_bytes + SRV_JOB_ARCHIVE_MAX > SRV_STORE_SCAN_BYTES ||
                srv_job_archive_read(job_id, archive, &request, &response) || store_json_refs(catalog, request) || store_json_refs(catalog, response)) { catalog->complete = 0; continue; }
            catalog->root_bytes += SRV_JOB_ARCHIVE_MAX;
            if (entry->kind == STORE_SPOOL && store_json_field(response, !strcmp(suffix, ".stdout") ? "stdout_ref" : "stderr_ref",
                                                             entry->reference, sizeof(entry->reference))) catalog->complete = 0;
            if (catalog->complete) catalog->root_error = NULL;
            continue;
        }
        char* data = store_load(catalog, entry);
        if (!data || memchr(data, 0, (size_t)entry->st.st_size)) { free(data); catalog->complete = 0; continue; }
        if (entry->kind == STORE_RECEIPT) {
            char encoded[16384], payload[12288];
            if (jef_verify_proof_bytes_pinned(data, (unsigned)entry->st.st_size, signer->pubkey_hex) != 0 ||
                store_json_field(data, "payload", encoded, sizeof(encoded))) catalog->complete = 0;
            else {
                long n = srv_b64_decode(encoded, strlen(encoded), (uint8_t*)payload, sizeof(payload) - 1);
                if (n < 0 || memchr(payload, 0, (size_t)(n < 0 ? 0 : n))) catalog->complete = 0;
                else {
                    payload[n] = 0;
                    if (store_json_refs(catalog, payload)) catalog->complete = 0;
                    char phase[32], state[32], digest[65];
                    if (!store_json_field(payload, "phase", phase, sizeof(phase)) && !strcmp(phase, "terminal") &&
                        !store_json_field(payload, "state", state, sizeof(state)) && !strcmp(state, "done") &&
                        strstr(payload, "\"quiescent\":true") &&
                        !store_json_field(payload, "start_request_blake3", digest, sizeof(digest)) && strlen(digest) == 64)
                        memcpy(entry->reference, digest, sizeof(digest));
                }
            }
        } else {
            char* first = strchr(data, '\n'); char* second = first ? strchr(first + 1, '\n') : NULL; char* replay = NULL;
            if (!first || first - data != 64 || !second) catalog->complete = 0;
            else if (second == first + 8 && !memcmp(first + 1, "running\n", 8)) {
                if (second[1]) {
                    if (srv_idem_appended_replay(second + 1, data + entry->st.st_size, &replay) != SRV_IDEM_REPLAY ||
                        store_json_refs(catalog, replay)) catalog->complete = 0;
                } else {
                    /* Jobs retain a running reservation even after terminal
                     * observations. A verified terminal receipt is evidence of
                     * quiescence, not permission to replay/rewrite that record. */
                    int resolved = 0;
                    for (size_t r = 0; r < catalog->count; r++)
                        if (catalog->entries[r].kind == STORE_RECEIPT && strlen(catalog->entries[r].reference) == 64 &&
                            !memcmp(catalog->entries[r].reference, data, 64)) resolved = 1;
                    if (!resolved) catalog->complete = 0;
                }
            } else catalog->complete = 0;
            free(replay);
        }
        free(data);
        if (catalog->complete) catalog->root_error = NULL;
    }
}

static int store_verify_entry(store_catalog_t* catalog, store_entry_t* entry, char hash[65]) {
    if (!S_ISREG(entry->st.st_mode) || entry->st.st_nlink != 1 || entry->st.st_uid != geteuid() ||
        (entry->st.st_mode & 0077) || entry->st.st_size < 0 ||
        catalog->checked_bytes + (uint64_t)entry->st.st_size > 2u * SRV_STORE_SCAN_BYTES) return -1;
    int fd = openat(catalog->fds[entry->directory], entry->name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    struct stat st;
    if (fd < 0) return -1;
    int ok = !fstat(fd, &st) && st.st_dev == entry->st.st_dev && st.st_ino == entry->st.st_ino && st.st_size == entry->st.st_size &&
        !srv_hash_regular_fd(fd, st.st_size, hash);
    close(fd); catalog->checked_bytes += (uint64_t)entry->st.st_size; return ok ? 0 : -1;
}

static int store_gc(store_catalog_t* catalog, const srv_job_req_t* request, jash_json_writer_t* out) {
    if (!catalog->complete) return -1;
    jash_recovery_pins_t* pins = jash_recovery_pins_load_home(srv_home());
    if (!jash_recovery_pins_complete(pins)) { jash_recovery_pins_free(pins); return -1; }
    int failed = 0; time_t now = time(NULL);
    for (size_t i = 0; i < catalog->count; i++) {
        store_entry_t* entry = &catalog->entries[i];
        if (entry->kind > STORE_STAGE && entry->kind != STORE_SPOOL) continue;
        char full[1400]; snprintf(full, sizeof(full), "%s/.jash/%s/%s", srv_home(), store_dirs[entry->directory], entry->name);
        if (jash_recovery_pins_contains(pins, full)) { entry->action = "retained_pin"; continue; }
        char expected[65], observed[65], reference[160] = ""; uint64_t size;
        int protected = entry->referenced;
        if (entry->kind == STORE_CAS || entry->kind == STORE_HOLD) {
            char canonical[1400]; snprintf(canonical, sizeof(canonical), "%s/.jash/overflow/%s", srv_home(), entry->reference);
            if (jash_recovery_pins_contains(pins, canonical)) protected = 1;
        }
        int restore = entry->kind == STORE_HOLD && protected;
        if (entry->kind == STORE_CAS && protected) { entry->action = "retained_reference"; continue; }
        if (!restore && (now < entry->st.st_mtime || (uint64_t)(now - entry->st.st_mtime) < (uint64_t)request->older_than_seconds)) { entry->action = "retained_age"; continue; }
        if (entry->kind == STORE_STAGE) {
            const char* original = entry->name + (!strncmp(entry->name, ".gc-", 4) ? 4 : 0);
            if (strlen(original) < 71 || original[69] != '-') continue;
            snprintf(reference, sizeof(reference), "blake3-%.*s-%llu.bin", 64, original + 5, (unsigned long long)entry->st.st_size);
        } else snprintf(reference, sizeof(reference), "%s", entry->reference);
        if (!store_cas_name(reference, expected, &size) || size != (uint64_t)entry->st.st_size) { entry->action = "verification_failed"; failed = 1; continue; }
        if (store_verify_entry(catalog, entry, observed) || strcmp(expected, observed)) { entry->action = "verification_failed"; failed = 1; continue; }
        char published[1400]; snprintf(published, sizeof(published), "%s/.jash/overflow/%s", srv_home(), reference);
        if (entry->kind == STORE_STAGE || entry->kind == STORE_SPOOL) {
            if (srv_cas_verify(published, entry->st.st_size, expected)) continue;
        }
        if (request->dry_run) { entry->action = restore ? "would_reconcile" : "would_remove"; catalog->planned++; continue; }
        jash_jsonw_raw(out, " ");
        if (jash_jsonw_finish(out) || srv_shutdown_requested()) { failed = 1; break; }
        int parent = catalog->fds[entry->directory];
        if (restore) {
            struct stat restored;
            if (srv_cas_verify(published, entry->st.st_size, expected)) {
                int moved = !jash_osif_renameat2(parent, entry->name, parent, reference, 1u);
                if (moved) catalog->changed = 1;
                if (moved &&
                    !fstatat(parent, reference, &restored, AT_SYMLINK_NOFOLLOW) &&
                    restored.st_dev == entry->st.st_dev && restored.st_ino == entry->st.st_ino) {
                    entry->action = "restored"; catalog->reconciled++;
                    if (fsync(parent)) { entry->action = "restored_sync_failed"; failed = 1; }
                } else { entry->action = "restore_failed"; failed = 1; }
                continue;
            }
            /* A valid canonical copy already exists: the held copy is redundant. */
        }
        char hold[300]; snprintf(hold, sizeof(hold), ".gc-%s", entry->name);
        int already_held = !strncmp(entry->name, ".gc-", 4);
        const char* remove = already_held ? entry->name : hold;
        if (!already_held && jash_osif_renameat2(parent, entry->name, parent, hold, 1u)) { entry->action = "capture_failed"; failed = 1; continue; }
        if (!already_held) catalog->changed = 1;
        entry->held = 1;
        struct stat captured;
        if (fstatat(parent, remove, &captured, AT_SYMLINK_NOFOLLOW) || captured.st_dev != entry->st.st_dev ||
            captured.st_ino != entry->st.st_ino || captured.st_nlink != 1 || captured.st_size != entry->st.st_size ||
            captured.st_mtime != entry->st.st_mtime) {
            entry->action = "identity_changed";
            if (!already_held && !jash_osif_renameat2(parent, remove, parent, entry->name, 1u)) entry->held = 0;
            failed = 1; continue;
        }
        if (unlinkat(parent, remove, 0)) { entry->action = "retained_gc_hold"; failed = 1; continue; }
        entry->held = 0; entry->action = "removed"; catalog->changed = 1;
        catalog->reclaimed++; catalog->reclaimed_bytes += (uint64_t)entry->st.st_size;
        if (fsync(parent)) { entry->action = "removed_sync_failed"; failed = 1; }
    }
    jash_recovery_pins_free(pins); return failed ? -1 : 0;
}

typedef struct {
    char name[256], original[256], hold[256], key[65], result[96];
    struct stat st;
    const char* action;
    int held;
} journal_gc_entry_t;

typedef struct {
    journal_gc_entry_t* entries;
    size_t count;
    unsigned scanned;
    uint64_t planned, reclaimed, reclaimed_bytes, verified_bytes;
    int attempted, complete, changed;
} journal_gc_report_t;

static int journal_gc_order(const void* a, const void* b) {
    return strcmp(((const journal_gc_entry_t*)a)->name, ((const journal_gc_entry_t*)b)->name);
}

static int journal_gc_private(const struct stat* st) {
    return S_ISREG(st->st_mode) && st->st_uid == geteuid() && !(st->st_mode & 0077) && st->st_nlink == 1 &&
        st->st_size > 0 && (uintmax_t)st->st_size <= STORE_MAINT_MAX;
}

static int journal_gc_captured(const struct stat* expected, const struct stat* current) {
    struct stat copy = *current;
    /* rename changes ctime; all other ownership/identity/content metadata must
     * agree, followed by another byte comparison and public-chain verification. */
#ifdef __APPLE__
    copy.st_ctimespec = expected->st_ctimespec;
#else
    copy.st_ctim = expected->st_ctim;
#endif
    return journal_gc_private(current) && maintenance_same_stat(expected, &copy);
}

static int journal_gc_same_result(const journal_gc_entry_t* entry, const char* name, const srv_receipt_signer_t* signer) {
    char* public = NULL; size_t size;
    if (maintenance_lookup(entry->key, NULL, signer, &public, NULL) != 1) return 0;
    char* staged = maintenance_read(name, STORE_MAINT_MAX, &size);
    int same = staged && size == (size_t)entry->st.st_size && strlen(public) == size && !memcmp(staged, public, size);
    free(staged); free(public); return same;
}

static void journal_gc_restore(int dir, journal_gc_entry_t* entry) {
    if (strcmp(entry->name, entry->original)) return; /* pre-existing hold */
    if (!jash_osif_renameat2(dir, entry->hold, dir, entry->original, 1u)) { entry->held = 0; (void)fsync(dir); }
}

static const char* maintenance_journal_gc(journal_gc_report_t* report, const srv_job_req_t* request,
                                        const srv_receipt_signer_t* signer, jash_json_writer_t* out) {
    report->attempted = 1;
    report->entries = calloc(SRV_STORE_MAX_FILES, sizeof(*report->entries));
    if (!report->entries) return "journal_gc_memory";
    int dir = srv_store_dir("agent_maintenance"); if (dir < 0) return "journal_gc_unavailable";
    int copy = openat(dir, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    DIR* directory = copy >= 0 ? fdopendir(copy) : NULL;
    if (!directory) { if (copy >= 0) close(copy); close(dir); return "journal_gc_unavailable"; }
    struct stat before, after; int failed = fstat(dir, &before) != 0;
    while (!failed) {
        errno = 0; struct dirent* item = readdir(directory);
        if (!item) { if (errno) failed = 1; break; }
        if (!strcmp(item->d_name, ".") || !strcmp(item->d_name, "..")) continue;
        if (report->scanned == SRV_STORE_MAX_FILES) { failed = 1; break; }
        report->scanned++;
        journal_gc_entry_t entry = {0};
        int held = maintenance_gc_name(item->d_name, entry.original, entry.key);
        if (held < 0) continue;
        snprintf(entry.name, sizeof(entry.name), "%s", item->d_name);
        int n = snprintf(entry.hold, sizeof(entry.hold), ".jgc-%s", entry.original + 1);
        if (n < 0 || (size_t)n >= sizeof(entry.hold) || fstatat(dir, entry.name, &entry.st, AT_SYMLINK_NOFOLLOW)) { failed = 1; break; }
        entry.held = held;
        snprintf(entry.result, sizeof(entry.result), "%s.result", entry.key);
        entry.action = "retained_unchecked";
        report->entries[report->count++] = entry;
    }
    closedir(directory);
    if (fstat(dir, &after) || (!failed && !maintenance_same_stat(&before, &after))) failed = 1;
    if (failed) { close(dir); return "journal_gc_scan_incomplete"; }
    report->complete = 1;
    qsort(report->entries, report->count, sizeof(*report->entries), journal_gc_order);
    jash_recovery_pins_t* pins = jash_recovery_pins_load_home(srv_home());
    if (!jash_recovery_pins_complete(pins)) { jash_recovery_pins_free(pins); close(dir); return "journal_gc_pins_incomplete"; }
    time_t now = time(NULL);
    for (size_t i = 0; i < report->count; i++) {
        journal_gc_entry_t* entry = &report->entries[i]; struct stat current, public;
        if (!journal_gc_private(&entry->st)) { entry->action = "retained_unsafe"; continue; }
        char path[1400], held_path[1400];
        snprintf(path, sizeof(path), "%s/.jash/agent_maintenance/%s", srv_home(), entry->original);
        snprintf(held_path, sizeof(held_path), "%s/.jash/agent_maintenance/%s", srv_home(), entry->hold);
        if (jash_recovery_pins_contains(pins, path) || jash_recovery_pins_contains(pins, held_path)) { entry->action = "retained_pin"; continue; }
        if (now < entry->st.st_mtime || (uint64_t)(now - entry->st.st_mtime) < (uint64_t)request->older_than_seconds) { entry->action = "retained_age"; continue; }
        if (fstatat(dir, entry->result, &public, AT_SYMLINK_NOFOLLOW) || !journal_gc_private(&public)) { entry->action = "retained_no_public_result"; continue; }
        if (public.st_size != entry->st.st_size) { entry->action = "retained_not_identical"; continue; }
        uint64_t charge = 6u * (uint64_t)entry->st.st_size + 262144u;
        if (charge > 2u * SRV_STORE_SCAN_BYTES - report->verified_bytes) { entry->action = "retained_verification_limit"; failed = 1; break; }
        report->verified_bytes += charge;
        if (fstatat(dir, entry->name, &current, AT_SYMLINK_NOFOLLOW) || !maintenance_same_stat(&entry->st, &current)) {
            entry->action = "identity_changed"; failed = 1; continue;
        }
        if (!journal_gc_same_result(entry, entry->name, signer)) { entry->action = "retained_unverified"; continue; }
        if (request->dry_run) { entry->action = "would_remove_redundant"; report->planned++; continue; }
        jash_jsonw_raw(out, " ");
        if (jash_jsonw_finish(out) || srv_shutdown_requested()) { entry->action = "interrupted"; failed = 1; break; }
        if (!entry->held) {
            if (jash_osif_renameat2(dir, entry->name, dir, entry->hold, 1u)) { entry->action = "capture_failed"; failed = 1; continue; }
            entry->held = 1; report->changed = 1;
            if (fsync(dir)) { entry->action = "capture_sync_failed"; failed = 1; continue; }
        }
        if (fstatat(dir, entry->hold, &current, AT_SYMLINK_NOFOLLOW) || !journal_gc_captured(&entry->st, &current)) {
            entry->action = "identity_changed"; journal_gc_restore(dir, entry); failed = 1; continue;
        }
        if (fstatat(dir, entry->result, &current, AT_SYMLINK_NOFOLLOW) || !maintenance_same_stat(&public, &current) ||
            !journal_gc_same_result(entry, entry->hold, signer)) {
            entry->action = "public_or_content_changed"; journal_gc_restore(dir, entry); failed = 1; continue;
        }
        if (unlinkat(dir, entry->hold, 0)) { entry->action = "unlink_failed"; failed = 1; continue; }
        entry->held = 0; report->changed = 1; report->reclaimed++; report->reclaimed_bytes += (uint64_t)entry->st.st_size;
        entry->action = "removed_redundant";
        if (fsync(dir)) { entry->action = "removed_sync_failed"; failed = 1; }
    }
    jash_recovery_pins_free(pins); close(dir); return failed ? "journal_gc_incomplete" : NULL;
}

static void maintenance_journal_gc_json(jash_json_writer_t* out, const journal_gc_report_t* report) {
    jash_jsonw_raw(out, ",\"journal_gc\":{\"quota_mode\":\"redundant-gc-exempt\",\"attempted\":"); jash_jsonw_raw(out, report->attempted ? "true" : "false");
    jash_jsonw_raw(out, ",\"scan_complete\":"); jash_jsonw_raw(out, report->complete ? "true" : "false");
    jash_jsonw_raw(out, ",\"entries_scanned\":"); jash_jsonw_safe_u64(out, report->scanned);
    jash_jsonw_raw(out, ",\"planned_files\":"); jash_jsonw_safe_u64(out, report->planned);
    jash_jsonw_raw(out, ",\"reclaimed_files\":"); jash_jsonw_safe_u64(out, report->reclaimed);
    jash_jsonw_raw(out, ",\"reclaimed_bytes\":"); jash_jsonw_safe_u64(out, report->reclaimed_bytes);
    jash_jsonw_raw(out, ",\"verification_bytes_charged\":"); jash_jsonw_safe_u64(out, report->verified_bytes);
    jash_jsonw_raw(out, ",\"items\":[");
    for (size_t i = 0; i < report->count && !out->error; i++) {
        const journal_gc_entry_t* entry = &report->entries[i]; if (i) jash_jsonw_raw(out, ",");
        jash_jsonw_raw(out, "{\"ref\":"); jash_jsonw_string(out, entry->name);
        jash_jsonw_raw(out, ",\"public_ref\":"); jash_jsonw_string(out, entry->result);
        jash_jsonw_raw(out, ",\"bytes\":"); if (entry->st.st_size >= 0) jash_jsonw_safe_u64(out, (uint64_t)entry->st.st_size); else jash_jsonw_raw(out, "null");
        jash_jsonw_raw(out, ",\"action\":"); jash_jsonw_string(out, entry->action);
        jash_jsonw_raw(out, ",\"retained_as\":"); if (entry->held) jash_jsonw_string(out, entry->hold); else jash_jsonw_raw(out, "null");
        jash_jsonw_raw(out, "}");
    }
    jash_jsonw_raw(out, "]}");
}

static void srv_store_command(int respfd, srv_job_table_t* table, const srv_job_req_t* request,
                               const srv_receipt_signer_t* signer, const char* text) {
    if (!strcmp(request->op, "store.history")) { srv_maintenance_history(respfd, request, signer); return; }
    if (!strcmp(request->op, "store.inspect") && (request->seen & SRV_JOB_SEEN_TARGET_ID)) { srv_maintenance_inspect(respfd, request, signer); return; }
    if (!strcmp(request->op, "store.resolve")) { srv_store_resolve(respfd, table, request, signer); return; }
    int exclusive = strcmp(request->op, "store.inspect") != 0, locked = 0, applied = 0;
    int journal_gc = !strcmp(request->op, "store.journal.gc"); journal_gc_report_t journal_report = {0};
    const char* error = NULL; const char* transaction = exclusive ? "not_started" : "observation";
    maintenance_tx_t tx = {0}; maintenance_buffer_t buffer = {.fd = respfd, .streaming = 1};
    if (exclusive) {
        maintenance_key(request->id_value, tx.key);
        char* replay = NULL;
        int status = maintenance_lookup(tx.key, text, signer, &replay, NULL);
        if (status == 1) { (void)srv_write_all(respfd, replay, strlen(replay)); free(replay); return; }
        if (status == 2) { error = "maintenance_indeterminate"; transaction = "indeterminate"; }
        if (status == 3) error = "maintenance_id_conflict";
        buffer.bytes = malloc(STORE_MAINT_MAX);
        if (!buffer.bytes) { srv_job_error(respfd, request, "out of memory"); return; }
        buffer.bytes[0] = 0;
    }
    jash_json_writer_t out; jash_jsonw_init(&out, exclusive ? maintenance_sink : jash_jsonw_fd_sink,
                                          exclusive ? (void*)&buffer : (void*)&respfd);
    jash_jsonw_raw(&out, "{\"schema\":\"jash.agent-store.v1\",\"id\":"); jash_jsonw_string(&out, request->id_value);
    jash_jsonw_raw(&out, ",\"op\":"); jash_jsonw_string(&out, request->op);
    if (jash_jsonw_finish(&out)) { free(buffer.bytes); return; }
    if (exclusive && !error) {
        for (int i = 0; i < SRV_JOB_MAX; i++) if (table->jobs[i].used) error = "session_has_jobs";
        char lease_path[1100]; struct stat lease;
        snprintf(lease_path, sizeof(lease_path), "%s/.jash/agent_store/lease", srv_home());
        if (!error && (fstat(srv_store_lease, &lease) || !srv_regular_identity_ok(lease_path, &lease))) error = "lease_identity_changed";
        if (!error && flock(srv_store_lease, LOCK_EX | LOCK_NB)) {
            error = "store_busy";
            if (flock(srv_store_lease, LOCK_SH)) { error = "lease_restore_failed"; srv_store_poisoned = 1; }
        } else if (!error) locked = 1;
    }
    int history_complete = !error && maintenance_history_complete(signer);
    if (!error && !history_complete && !journal_gc) error = "maintenance_incomplete";
    uint64_t journal_limit = 0, journal_used = 0, journal_available = 0;
    unsigned journal_entries = 0; int journal_space_known = 0;
    int journal_policy_known = !srv_store_policy_limit("journal_quota", &journal_limit);
    int journal_measured = !maintenance_journal_usage(&journal_used, &journal_entries);
    int journal_policy_change = !strcmp(request->op, "store.journal.quota");
    if (!error && !journal_policy_known && !journal_gc) error = "journal_policy_invalid";
    if (exclusive && !error && !journal_gc) {
        uint64_t proposed = journal_policy_change ? (uint64_t)request->max_bytes : journal_limit;
        error = maintenance_journal_admit(proposed, journal_used, journal_entries, journal_measured,
                                          &journal_available, &journal_space_known);
    }
    if (exclusive && !error) {
        if (maintenance_begin(&tx, request, text, signer)) { error = "maintenance_prepare_failed"; transaction = "indeterminate"; history_complete = 0; }
        else transaction = "committed";
    }
    uint64_t limit = 0, used = 0;
    int have_limit = !srv_store_limit(&limit), have_used = !srv_store_usage(&used);
    if (!error && (!have_limit || !have_used) && !journal_gc) error = "measurement_incomplete";
    if (!error && !strcmp(request->op, "store.quota")) {
        uint64_t proposed = (uint64_t)request->max_bytes;
        if (proposed && proposed < used) error = "quota_below_usage";
        else if (!request->dry_run) { error = srv_store_policy_publish("quota", proposed, &applied); if (applied) limit = proposed; }
    }
    if (!error && journal_policy_change && !request->dry_run) {
        error = srv_store_policy_publish("journal_quota", (uint64_t)request->max_bytes, &applied);
        if (applied) journal_limit = (uint64_t)request->max_bytes;
    }
    store_catalog_t catalog;
    if (journal_gc) { memset(&catalog, 0, sizeof(catalog)); for (int i = 0; i < 5; i++) catalog.fds[i] = -1; }
    else store_catalog_scan(&catalog, signer);
    if (!error && !catalog.complete && strcmp(request->op, "store.quota") && !journal_policy_change && !journal_gc) error = "roots_incomplete";
    if (!error && !strcmp(request->op, "store.release")) {
        store_entry_t* target = NULL;
        for (size_t i = 0; i < catalog.count; i++)
            if (!strcmp(store_dirs[catalog.entries[i].directory], request->root_namespace) && !strcmp(catalog.entries[i].name, request->root_name)) target = &catalog.entries[i];
        if (!target || !target->root_hash[0]) error = "root_unavailable";
        else if (strcmp(target->root_hash, request->root_blake3)) error = "root_changed";
        else if (target->released) target->action = "already_released";
        else if (request->dry_run) { target->action = "would_release_retention"; catalog.planned++; }
        else {
            char name[96], marker[66]; maintenance_release_name(request->root_namespace, request->root_name, name);
            snprintf(marker, sizeof(marker), "%s\n", tx.key);
            int published = maintenance_publish(name, marker, 65, 1);
            if (published) { error = "release_publish_failed"; if (published > 0) { applied = 1; target->action = "release_marker_unconfirmed"; } }
            else { target->action = "release_marker_written"; applied = 1; }
        }
    }
    if (!error && !strcmp(request->op, "store.gc")) {
        if (store_gc(&catalog, request, &out)) error = "gc_incomplete";
        applied = catalog.changed;
        have_used = !srv_store_usage(&used);
    }
    if (!error && journal_gc) {
        error = maintenance_journal_gc(&journal_report, request, signer, &out);
        applied = journal_report.changed;
    }
    if (exclusive) {
        if (buffer.channel_failed) error = "delivery_failed";
        buffer.streaming = 0;
        jash_jsonw_init(&out, maintenance_sink, &buffer);
    }
    jash_jsonw_raw(&out, ",\"quota_scope\":\"cas-and-job-archives-including-staging\",\"quota_bytes\":");
    if (have_limit) jash_jsonw_safe_u64(&out, limit); else jash_jsonw_raw(&out, "null");
    jash_jsonw_raw(&out, ",\"used_bytes\":");
    if (have_used) jash_jsonw_safe_u64(&out, used); else jash_jsonw_raw(&out, "null");
    jash_jsonw_raw(&out, ",\"journal_quota_scope\":\"maintenance-regular-files-including-staging\",\"journal_quota_bytes\":");
    if (journal_policy_known) jash_jsonw_safe_u64(&out, journal_limit); else jash_jsonw_raw(&out, "null");
    jash_jsonw_raw(&out, ",\"journal_used_bytes\":"); if (journal_measured) jash_jsonw_safe_u64(&out, journal_used); else jash_jsonw_raw(&out, "null");
    jash_jsonw_raw(&out, ",\"journal_usage_phase\":\"before-operation\",\"journal_measurement_complete\":"); jash_jsonw_raw(&out, journal_measured ? "true" : "false");
    jash_jsonw_raw(&out, ",\"journal_entries\":"); jash_jsonw_safe_u64(&out, journal_entries);
    jash_jsonw_raw(&out, ",\"journal_required_headroom_bytes\":"); jash_jsonw_safe_u64(&out, STORE_JOURNAL_HEADROOM);
    jash_jsonw_raw(&out, ",\"journal_fs_available_bytes\":"); if (journal_space_known) jash_jsonw_safe_u64(&out, journal_available); else jash_jsonw_raw(&out, "null");
    jash_jsonw_raw(&out, ",\"dry_run\":"); jash_jsonw_raw(&out, request->dry_run ? "true" : "false");
    jash_jsonw_raw(&out, ",\"applied\":"); jash_jsonw_raw(&out, applied ? "true" : "false");
    jash_jsonw_raw(&out, ",\"root_retention\":\"explicit-release\",\"scan_complete\":"); jash_jsonw_raw(&out, catalog.complete ? "true" : "false");
    jash_jsonw_raw(&out, ",\"root_error\":"); if (catalog.root_error) jash_jsonw_string_or_bytes(&out, catalog.root_error); else jash_jsonw_raw(&out, "null");
    jash_jsonw_raw(&out, ",\"coordination\":"); jash_jsonw_string(&out, locked ? "exclusive" : "shared");
    jash_jsonw_raw(&out, ",\"entries_scanned\":"); jash_jsonw_safe_u64(&out, catalog.count);
    jash_jsonw_raw(&out, ",\"reclaimed_files\":"); jash_jsonw_safe_u64(&out, catalog.reclaimed);
    jash_jsonw_raw(&out, ",\"reclaimed_bytes\":"); jash_jsonw_safe_u64(&out, catalog.reclaimed_bytes);
    jash_jsonw_raw(&out, ",\"planned_files\":"); jash_jsonw_safe_u64(&out, catalog.planned);
    jash_jsonw_raw(&out, ",\"reconciled_files\":"); jash_jsonw_safe_u64(&out, catalog.reconciled);
    static const char* const kinds[] = {"cas", "gc_hold", "cas_staging", "job_archive", "receipt", "idempotency", "spool", "other"};
    jash_jsonw_raw(&out, ",\"items\":[");
    for (size_t i = 0; i < catalog.count; i++) {
        store_entry_t* entry = &catalog.entries[i];
        if (i) jash_jsonw_raw(&out, ",");
        jash_jsonw_raw(&out, "{\"namespace\":"); jash_jsonw_string(&out, store_dirs[entry->directory]);
        jash_jsonw_raw(&out, ",\"name\":"); jash_jsonw_string_or_bytes(&out, entry->name);
        jash_jsonw_raw(&out, ",\"kind\":"); jash_jsonw_string(&out, kinds[entry->kind]);
        jash_jsonw_raw(&out, ",\"root_blake3\":"); if (entry->root_hash[0]) jash_jsonw_string(&out, entry->root_hash); else jash_jsonw_raw(&out, "null");
        jash_jsonw_raw(&out, ",\"retention\":"); jash_jsonw_string(&out, !entry->root_hash[0] ? "not-root" : entry->released ? "released" : "kept");
        jash_jsonw_raw(&out, ",\"bytes\":"); if (entry->metadata_valid && entry->st.st_size >= 0) jash_jsonw_safe_u64(&out, (uint64_t)entry->st.st_size); else jash_jsonw_raw(&out, "null");
        const char* action = entry->action ? entry->action : !entry->metadata_valid ? "unreadable" :
            entry->referenced ? "retained_reference" : !catalog.complete ? "retained_incomplete_scan" :
            entry->kind == STORE_CAS ? "unreferenced_candidate" : "retained";
        jash_jsonw_raw(&out, ",\"action\":"); jash_jsonw_string(&out, action);
        if (entry->held) {
            char name[300]; snprintf(name, sizeof(name), "%s%s", !strncmp(entry->name, ".gc-", 4) ? "" : ".gc-", entry->name);
            jash_jsonw_raw(&out, ",\"retained_as\":"); jash_jsonw_string_or_bytes(&out, name);
        }
        jash_jsonw_raw(&out, "}");
    }
    jash_jsonw_raw(&out, "]");
    if (journal_gc) maintenance_journal_gc_json(&out, &journal_report);
    if (exclusive && jash_jsonw_finish(&out)) error = "maintenance_result_limit";
    size_t verdict = buffer.used;
    jash_jsonw_raw(&out, ",\"effect_admission\":"); jash_jsonw_string(&out, tx.begun ? "admitted" : "unknown");
    jash_jsonw_raw(&out, ",\"transaction\":"); jash_jsonw_string(&out, transaction);
    jash_jsonw_raw(&out, ",\"maintenance_complete\":"); jash_jsonw_raw(&out, history_complete ? "true" : "false");
    jash_jsonw_raw(&out, ",\"ok\":"); jash_jsonw_raw(&out, error ? "false" : "true");
    jash_jsonw_raw(&out, ",\"error\":"); if (error) jash_jsonw_string(&out, error); else jash_jsonw_raw(&out, "null");
    jash_jsonw_raw(&out, "}\n"); (void)jash_jsonw_finish(&out);
    if (exclusive) {
        if (tx.begun && (out.error || maintenance_commit(&tx, request, &buffer, signer))) {
            buffer.used = verdict; buffer.bytes[buffer.used] = 0;
            jash_jsonw_init(&out, maintenance_sink, &buffer);
            jash_jsonw_raw(&out, ",\"transaction\":\"indeterminate\",\"maintenance_complete\":false,\"ok\":false,\"error\":\"maintenance_commit_failed\"}\n");
            (void)jash_jsonw_finish(&out);
        }
        if (locked && flock(srv_store_lease, LOCK_SH)) srv_store_poisoned = 1;
        if (!buffer.channel_failed && !out.error && buffer.used >= buffer.sent)
            (void)srv_write_all(respfd, buffer.bytes + buffer.sent, buffer.used - buffer.sent);
        free(buffer.bytes);
    }
    store_catalog_free(&catalog);
    free(journal_report.entries);
}

static int srv_response_closed(int fd) {
    struct pollfd peer = {.fd = fd, .events = POLLOUT};
    return poll(&peer, 1, 0) > 0 && (peer.revents & (POLLHUP | POLLERR | POLLNVAL));
}

static void srv_job_status(int respfd, srv_job_table_t* table,
                           const srv_job_req_t* request,
                           const srv_receipt_signer_t* signer) {
    srv_job_t* job = srv_job_find(table, request->job_id);
    if (!job) {
        srv_job_error(respfd, request, "job not found");
        return;
    }
    int wait_result = 0;
    char extra[160] = "";
    if (strcmp(request->op, "job.wait") == 0) {
        long remaining = request->wait_ms;
        do {
            if (srv_shutdown_requested() || srv_response_closed(respfd)) return;
            int slice = remaining > 50 ? 50 : (int)remaining;
            wait_result = jash_agent_process_wait(job->process, slice);
            remaining -= slice;
        } while (wait_result == 1 && remaining > 0);
        if (wait_result < 0) {
            srv_job_error(respfd, request, "job wait failed");
            return;
        }
        snprintf(extra, sizeof extra, "\"timed_out\":%s",
                 wait_result == 1 ? "true" : "false");
    } else if (srv_job_refresh(job) != 0) {
        srv_job_error(respfd, request, "job poll failed");
        return;
    }
    if (srv_job_finalize_if_done(respfd, request, job) != 0) return;
    char response[4096];
    int formatted = srv_job_response(request, job, signer, extra,
                                     response, sizeof response);
    if (formatted == -2)
        srv_dprintf(respfd,
                "{\"id\":%s,\"protocol_version\":2,\"op\":\"%s\","
                "\"error\":\"terminal receipt commit failed\","
                "\"outcome\":\"indeterminate\"}\n",
                request->id, request->op);
    else if (formatted != 0)
        srv_job_error(respfd, request, "job response overflow");
    else
        (void)srv_write_all(respfd, response, strlen(response));
}

static void srv_job_cancel(int respfd, srv_job_table_t* table,
                           const srv_job_req_t* request,
                           const char* text,
                           const srv_receipt_signer_t* signer) {
    srv_idem_t reservation;
    int idem = srv_job_idem_begin(respfd, request, text, &reservation);
    if (idem != 0) return;
    srv_job_t* job = srv_job_find(table, request->job_id);
    if (!job) {
        srv_job_idem_reject_request(respfd, &reservation, request,
                                    "job not found");
        return;
    }
    const jash_agent_process_result_t* result =
        jash_agent_process_get_result(job->process);
    if (!result) {
        srv_job_idem_indeterminate(respfd, &reservation, request,
                                   "job state unavailable", 0, 0);
        return;
    }
    if (job->cancel_indeterminate &&
        result->state != JASH_AGENT_PROCESS_DONE) {
        srv_job_idem_indeterminate(respfd, &reservation, request,
                                   "prior job cancellation outcome unknown",
                                   0, 0);
        return;
    }
    if (srv_job_refresh(job) != 0) {
        srv_job_idem_reject_request(respfd, &reservation, request,
                                    "job poll failed");
        return;
    }
    result = jash_agent_process_get_result(job->process);
    int applied = !job->cancel_attempted &&
                  result->state != JASH_AGENT_PROCESS_DONE;
    if (applied) {
        job->cancel_attempted = 1;
        if (jash_agent_process_cancel(job->process,
                                      (unsigned)request->grace_ms) != 0) {
            job->cancel_indeterminate = 1;
            srv_job_idem_indeterminate(respfd, &reservation, request,
                                       "job cancellation failed", 0, 0);
            return;
        }
        job->cancel_applied = 1;
    }
    if (srv_job_finalize_if_done(respfd, request, job) != 0) {
        if (reservation.fd >= 0) close(reservation.fd);
        return;
    }
    char extra[96], response[4096];
    snprintf(extra, sizeof extra, "\"cancel_applied\":%s",
             applied ? "true" : "false");
    int formatted = srv_job_response(request, job, signer, extra,
                                     response, sizeof response);
    if (formatted != 0) {
        srv_job_idem_indeterminate(respfd, &reservation, request,
            formatted == -2 ? "terminal receipt commit failed"
                            : "job response overflow", 0, 0);
        return;
    }
    (void)srv_job_idem_commit(respfd, &reservation, request, response);
}

static void srv_job_resize(int respfd, srv_job_table_t* table,
                           const srv_job_req_t* request, const char* text,
                           const srv_receipt_signer_t* signer) {
    srv_idem_t reservation;
    int idem = srv_job_idem_begin(respfd, request, text, &reservation);
    if (idem != 0) return;
    srv_job_t* job = srv_job_find(table, request->job_id);
    if (!job) {
        srv_job_idem_reject_request(respfd, &reservation, request,
                                    "job not found");
        return;
    }
    if (srv_job_refresh(job) != 0) {
        srv_job_idem_reject_request(respfd, &reservation, request,
                                    "job poll failed");
        return;
    }
    const jash_agent_process_result_t* result =
        jash_agent_process_get_result(job->process);
    if (!job->pty) {
        srv_job_idem_reject_request(respfd, &reservation, request,
                                    "job has no PTY");
        return;
    }
    if (result->state == JASH_AGENT_PROCESS_DONE) {
        srv_job_idem_reject_request(respfd, &reservation, request,
                                    "job is already terminal");
        return;
    }
    if (jash_agent_process_resize_pty(job->process,
                                      request->rows, request->cols) != 0) {
        srv_job_idem_indeterminate(respfd, &reservation, request,
                                   "PTY resize failed", 0, 0);
        return;
    }
    char extra[128], response[2048];
    snprintf(extra, sizeof extra, "\"rows\":%u,\"cols\":%u",
             request->rows, request->cols);
    if (srv_job_format(request, job, extra, response, sizeof response) != 0 ||
        srv_job_attach_action_receipt(signer, request, job, extra,
                                      response, sizeof response) != 0) {
        srv_job_idem_indeterminate(respfd, &reservation, request,
                                   "action receipt commit failed", 0, 0);
        return;
    }
    (void)srv_job_idem_commit(respfd, &reservation, request, response);
}

static void srv_job_write(int respfd, srv_job_table_t* table,
                          const srv_job_req_t* request, const char* text,
                          const srv_receipt_signer_t* signer) {
    srv_idem_t reservation;
    int idem = srv_job_idem_begin(respfd, request, text, &reservation);
    if (idem != 0) return;
    srv_job_t* job = srv_job_find(table, request->job_id);
    if (!job) {
        srv_job_idem_reject_request(respfd, &reservation, request,
                                    "job not found");
        return;
    }
    if (srv_job_refresh(job) != 0) {
        srv_job_idem_reject_request(respfd, &reservation, request,
                                    "job poll failed");
        return;
    }
    const jash_agent_process_result_t* result =
        jash_agent_process_get_result(job->process);
    if (!job->pty && !job->stdin_pipe) {
        srv_job_idem_reject_request(respfd, &reservation, request,
                                    "job has no PTY");
        return;
    }
    if (!job->pty && job->stdin_closed) {
        srv_job_idem_reject_request(respfd, &reservation, request,
                                    "job stdin is closed");
        return;
    }
    const char* write_cap = job->pty ? "job:pty:write" : "job:stdin:write";
    if (!srv_job_has_cap(request, write_cap)) {
        srv_job_idem_reject_request(respfd, &reservation, request,
                                    "capability denied");
        return;
    }
    if (result->state == JASH_AGENT_PROCESS_DONE) {
        srv_job_idem_reject_request(respfd, &reservation, request,
                                    "job is already terminal");
        return;
    }
    size_t accepted = 0;
    int written = job->pty
        ? jash_agent_process_write_pty(job->process, request->write_data,
                                      request->write_len,
                                      (int)request->wait_ms, &accepted)
        : jash_agent_process_write_stdin(job->process, request->write_data,
                                        request->write_len,
                                        (int)request->wait_ms, &accepted);
    if (written != 0) {
        char error[256];
        snprintf(error, sizeof error, "%s: %s",
                 written == 1 ? "job write deadline expired"
                              : "job write failed",
                 strerror(errno));
        if (accepted > 0) {
            srv_job_idem_indeterminate(respfd, &reservation, request,
                                       error, accepted, 1);
        } else {
            srv_job_idem_reject_request(respfd, &reservation, request, error);
        }
        return;
    }
    char extra[64], response[2048];
    snprintf(extra, sizeof extra, "\"accepted_bytes\":%zu", accepted);
    if (srv_job_format(request, job, extra, response, sizeof response) != 0 ||
        srv_job_attach_action_receipt(signer, request, job, extra,
                                      response, sizeof response) != 0) {
        srv_job_idem_indeterminate(respfd, &reservation, request,
                                   "action receipt commit failed",
                                   accepted, 1);
        return;
    }
    (void)srv_job_idem_commit(respfd, &reservation, request, response);
}

static void srv_job_close_stdin(int respfd, srv_job_table_t* table,
                                const srv_job_req_t* request, const char* text,
                                const srv_receipt_signer_t* signer) {
    srv_idem_t reservation;
    if (srv_job_idem_begin(respfd, request, text, &reservation) != 0) return;
    srv_job_t* job = srv_job_find(table, request->job_id);
    if (!job) {
        srv_job_idem_reject_request(respfd, &reservation, request,
                                    "job not found");
        return;
    }
    if (!job->stdin_pipe) {
        srv_job_idem_reject_request(respfd, &reservation, request,
                                    "job has no writable stdin pipe");
        return;
    }
    if (srv_job_refresh(job) != 0) {
        srv_job_idem_reject_request(respfd, &reservation, request,
                                    "job poll failed");
        return;
    }
    const jash_agent_process_result_t* result =
        jash_agent_process_get_result(job->process);
    if (result && (result->state == JASH_AGENT_PROCESS_DONE ||
                   result->cancel_requested))
        job->stdin_closed = 1;
    int applied = !job->stdin_closed;
    if (applied && jash_agent_process_close_stdin(job->process) != 0) {
        srv_job_idem_indeterminate(respfd, &reservation, request,
                                   "stdin half-close failed", 0, 0);
        return;
    }
    job->stdin_closed = 1;
    char extra[64], response[2048];
    snprintf(extra, sizeof extra, "\"close_applied\":%s",
             applied ? "true" : "false");
    if (srv_job_format(request, job, extra, response, sizeof response) != 0 ||
        srv_job_attach_action_receipt(signer, request, job, extra,
                                      response, sizeof response) != 0) {
        srv_job_idem_indeterminate(respfd, &reservation, request,
                                   "action receipt commit failed", 0, 0);
        return;
    }
    (void)srv_job_idem_commit(respfd, &reservation, request, response);
}

static void srv_job_read(int respfd, srv_job_table_t* table,
                         const srv_job_req_t* request) {
    srv_job_t* job = srv_job_find(table, request->job_id);
    if (!job) {
        srv_job_error(respfd, request, "job not found");
        return;
    }
    if (srv_job_refresh(job) != 0) {
        srv_job_error(respfd, request, "job poll failed");
        return;
    }
    if (srv_job_finalize_if_done(respfd, request, job) != 0) return;
    const jash_agent_process_result_t* result =
        jash_agent_process_get_result(job->process);
    int terminal = result->state == JASH_AGENT_PROCESS_DONE;
    char cas_path[1100];
    const char* path;
    const char* expected_hash = NULL;
    unsigned long long expected_bytes = 0;
    if (terminal) {
        char cas_dir[896];
        const char* ref = strcmp(request->stream, "stdout") == 0
            ? job->stdout_ref : job->stderr_ref;
        expected_hash = strcmp(request->stream, "stdout") == 0
            ? job->stdout_hash : job->stderr_hash;
        expected_bytes = strcmp(request->stream, "stdout") == 0
            ? job->stdout_bytes : job->stderr_bytes;
        int pn = srv_private_dir("overflow", cas_dir, sizeof cas_dir) == 0
            ? snprintf(cas_path, sizeof cas_path, "%s/%s", cas_dir, ref) : -1;
        if (pn <= 0 || (size_t)pn >= sizeof cas_path ||
            srv_cas_verify(cas_path, (off_t)expected_bytes,
                           expected_hash) != 0) {
            srv_job_error(respfd, request, "terminal CAS unavailable");
            return;
        }
        path = cas_path;
    } else {
        path = strcmp(request->stream, "stdout") == 0
            ? job->stdout_path : job->stderr_path;
    }
    struct stat st;
    int fd = srv_open_regular_nofollow(path, &st);
    if (fd < 0 || (st.st_mode & 077) != 0 || st.st_size < 0) {
        if (fd >= 0) close(fd);
        srv_job_error(respfd, request, "private job spool unavailable");
        return;
    }
    if (terminal) {
        dev_t expected_dev = strcmp(request->stream, "stdout") == 0
            ? job->stdout_dev : job->stderr_dev;
        ino_t expected_ino = strcmp(request->stream, "stdout") == 0
            ? job->stdout_ino : job->stderr_ino;
        if (st.st_dev != expected_dev || st.st_ino != expected_ino) {
            close(fd);
            srv_job_error(respfd, request, "terminal CAS identity changed");
            return;
        }
    }
    if ((off_t)request->offset > st.st_size) {
        close(fd);
        srv_job_error(respfd, request, "offset exceeds current stream size");
        return;
    }
    size_t available = (size_t)(st.st_size - (off_t)request->offset);
    size_t wanted = (size_t)request->limit;
    if (wanted > available) wanted = available;
    uint8_t* bytes = malloc(wanted ? wanted : 1u);
    if (!bytes) {
        close(fd);
        srv_job_error(respfd, request, "out of memory");
        return;
    }
    size_t received = 0;
    while (received < wanted) {
        ssize_t count = pread(fd, bytes + received, wanted - received,
                              (off_t)request->offset + (off_t)received);
        if (count > 0) received += (size_t)count;
        else if (count < 0 && errno == EINTR) continue;
        else break;
    }
    close(fd);
    if (received != wanted) {
        free(bytes);
        srv_job_error(respfd, request, "job spool read failed");
        return;
    }
    size_t encoded_cap = 4u * ((received + 2u) / 3u) + 1u;
    char* encoded = malloc(encoded_cap);
    size_t response_cap = encoded_cap + 1024u;
    char* response = malloc(response_cap);
    if (!encoded || !response) {
        free(bytes);
        free(encoded);
        free(response);
        srv_job_error(respfd, request, "out of memory");
        return;
    }
    srv_b64(bytes, received, encoded);
    free(bytes);
    long next = request->offset + (long)received;
    int eof = terminal && next >= st.st_size;
    int n = snprintf(response, response_cap,
        "{\"id\":%s,\"protocol_version\":2,\"op\":\"job.read\","
        "\"job_id\":\"%s\",\"stream\":\"%s\",\"offset\":%ld,"
        "\"next_offset\":%ld,\"bytes\":%zu,\"eof\":%s,"
        "\"data_b64\":\"%s\"}\n",
        request->id, job->job_id, request->stream, request->offset,
        next, received, eof ? "true" : "false", encoded);
    free(encoded);
    if (n <= 0 || (size_t)n >= response_cap)
        srv_job_error(respfd, request, "job read response overflow");
    else
        (void)srv_write_all(respfd, response, (size_t)n);
    free(response);
}

static int srv_stream_ref_shape(const char* ref) {
    if (!ref || strlen(ref) != 51 || ref[0] != 's' || ref[1] != '-' ||
        ref[34] != '-') return 0;
    for (int i = 2; i < 51; i++)
        if (i != 34 && !strchr("0123456789abcdef", ref[i])) return 0;
    return 1;
}

static srv_data_stream_t* srv_stream_find(const char* ref) {
    for (int i = 0; i < SRV_STREAM_SLOTS; i++)
        if (srv_data_streams[i].used && !strcmp(srv_data_streams[i].ref, ref))
            return &srv_data_streams[i];
    return NULL;
}

static int srv_stream_new_ref(char out[64]) {
    if (srv_stream_sequence == UINT64_MAX) { errno = EOVERFLOW; return -1; }
    unsigned char random[16];
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    size_t used = 0;
    while (used < sizeof random) {
        ssize_t n = read(fd, random + used, sizeof random - used);
        if (n > 0) used += (size_t)n;
        else if (n < 0 && errno == EINTR) continue;
        else break;
    }
    close(fd);
    if (used != sizeof random) { errno = EIO; return -1; }
    char hex[33];
    srv_hex(random, sizeof random, hex);
    snprintf(out, 64, "s-%s-%016llx", hex,
             (unsigned long long)++srv_stream_sequence);
    return 0;
}

static const char* srv_stream_source(srv_job_t* job, const char* stream,
                                     int verify_before_open,
                                     int* source_fd, uint64_t* size,
                                     int* terminal, unsigned* final_flags,
                                     const char** final_ref,
                                     const char** final_hash) {
    const jash_agent_process_result_t* result =
        jash_agent_process_get_result(job->process);
    if (!result) return "job state unavailable";
    *terminal = result->state == JASH_AGENT_PROCESS_DONE;
    *final_flags = JASH_IPC_STREAM_EOF;
    if (result->output_limit_hit || result->resource_timed_out ||
        result->resource_violation || result->descendants_forced ||
        result->quiescence_forced)
        *final_flags |= JASH_IPC_STREAM_INCOMPLETE;
    if (result->cancel_requested)
        *final_flags |= JASH_IPC_STREAM_INCOMPLETE | JASH_IPC_STREAM_CANCELLED;
    int stdout_stream = !strcmp(stream, "stdout");
    const char* path = stdout_stream ? job->stdout_path : job->stderr_path;
    char cas_path[1100];
    uint64_t expected = stdout_stream ? job->stdout_bytes : job->stderr_bytes;
    *final_ref = stdout_stream ? job->stdout_ref : job->stderr_ref;
    *final_hash = stdout_stream ? job->stdout_hash : job->stderr_hash;
    if (*terminal) {
        char directory[896];
        int n = srv_private_dir("overflow", directory, sizeof directory) == 0
            ? snprintf(cas_path, sizeof cas_path, "%s/%s", directory, *final_ref)
            : -1;
        /* The stdin adapter verifies the full commitment while copying the
         * selected range. Other consumers retain eager CAS verification. */
        if (n <= 0 || (size_t)n >= sizeof cas_path ||
            (verify_before_open &&
             srv_cas_verify(cas_path, (off_t)expected, *final_hash) != 0))
            return "terminal CAS unavailable";
        path = cas_path;
    }
    struct stat st;
    int fd = srv_open_regular_nofollow(path, &st);
    if (fd < 0 || (st.st_mode & 077) || st.st_size < 0) {
        if (fd >= 0) close(fd);
        return "private job spool unavailable";
    }
    if (*terminal) {
        dev_t device = stdout_stream ? job->stdout_dev : job->stderr_dev;
        ino_t inode = stdout_stream ? job->stdout_ino : job->stderr_ino;
        if (st.st_dev != device || st.st_ino != inode ||
            (uint64_t)st.st_size != expected) {
            close(fd);
            return "terminal CAS identity changed";
        }
    }
    *source_fd = fd;
    *size = (uint64_t)st.st_size;
    return NULL;
}

static void srv_stream_open(int respfd, srv_job_table_t* table,
                            const srv_job_req_t* request) {
    if (srv_data_fd < 0) { srv_job_error(respfd, request, "data lane unavailable"); return; }
    srv_job_t* job = srv_job_find(table, request->job_id);
    if (!job) { srv_job_error(respfd, request, "job not found"); return; }
    if (srv_job_refresh(job) != 0) { srv_job_error(respfd, request, "job poll failed"); return; }
    if (srv_job_finalize_if_done(respfd, request, job) != 0) return;
    int fd = -1, terminal = 0; uint64_t size = 0; unsigned flags = 0;
    const char *final_ref = NULL, *final_hash = NULL;
    const char* error = srv_stream_source(job, request->stream, 1, &fd, &size,
                                          &terminal, &flags, &final_ref, &final_hash);
    (void)flags; (void)final_ref; (void)final_hash;
    if (error) { srv_job_error(respfd, request, error); return; }
    close(fd);
    if ((uint64_t)request->offset > size) {
        srv_job_error(respfd, request, "offset exceeds current stream size");
        return;
    }
    int slot = -1;
    for (int i = 0; i < SRV_STREAM_SLOTS; i++)
        if (!srv_data_streams[i].used) { slot = i; break; }
    if (slot < 0) { srv_job_error(respfd, request, "stream slots exhausted"); return; }
    srv_data_stream_t candidate = {0};
    if (srv_stream_new_ref(candidate.ref) != 0) {
        srv_job_error(respfd, request, "stream reference generation failed");
        return;
    }
    candidate.used = 1;
    candidate.offset = candidate.acknowledged = (uint64_t)request->offset;
    snprintf(candidate.job_id, sizeof candidate.job_id, "%s", job->job_id);
    snprintf(candidate.stream, sizeof candidate.stream, "%s", request->stream);
    srv_data_streams[slot] = candidate;
    srv_dprintf(respfd,
        "{\"schema\":\"jash.agent-stream.v1\",\"id\":%s,\"protocol_version\":2,"
        "\"op\":\"data.stream.open\",\"job_id\":\"%s\",\"stream\":\"%s\","
        "\"stream_ref\":\"%s\",\"offset\":%llu,\"source_size_bytes\":%llu,"
        "\"source_terminal\":%s,\"delivery\":\"pull-credit\",\"ok\":true}\n",
        request->id, job->job_id, request->stream, candidate.ref,
        (unsigned long long)candidate.offset, (unsigned long long)size,
        terminal ? "true" : "false");
}

static void srv_stream_result(int respfd, const srv_job_req_t* request,
                              const srv_data_stream_t* stream, int packet_sent,
                              int blocked, int credit_applied, size_t bytes,
                              uint64_t sent_sequence, uint64_t sent_offset,
                              int eof, int complete, const char* final_ref,
                              const char* final_hash) {
    srv_dprintf(respfd,
        "{\"schema\":\"jash.agent-stream.v1\",\"id\":%s,\"protocol_version\":2,"
        "\"op\":\"data.stream.credit\",\"stream_ref\":\"%s\","
        "\"packet_sent\":%s,\"blocked\":%s,\"credit_applied\":%s,"
        "\"sequence\":%llu,\"offset\":%llu,\"bytes\":%zu,"
        "\"next_sequence\":%llu,\"next_offset\":%llu,"
        "\"ack_offset\":%llu,\"credit_available_bytes\":%llu,"
        "\"in_flight_bytes\":%llu,\"eof\":%s,\"complete\":%s,"
        "\"final_ref\":%s%s%s,\"final_blake3\":%s%s%s,\"ok\":true}\n",
        request->id, stream->ref, packet_sent ? "true" : "false",
        blocked ? "true" : "false", credit_applied ? "true" : "false",
        (unsigned long long)sent_sequence, (unsigned long long)sent_offset, bytes,
        (unsigned long long)stream->sequence, (unsigned long long)stream->offset,
        (unsigned long long)stream->acknowledged, (unsigned long long)stream->credit,
        (unsigned long long)stream->in_flight, eof ? "true" : "false",
        complete ? "true" : "false",
        final_ref ? "\"" : "", final_ref ? final_ref : "null", final_ref ? "\"" : "",
        final_hash ? "\"" : "", final_hash ? final_hash : "null", final_hash ? "\"" : "");
}

static void srv_stream_credit(int respfd, srv_job_table_t* table,
                              const srv_job_req_t* request) {
    srv_data_stream_t* stream = srv_stream_find(request->stream_ref);
    if (!stream) { srv_job_error(respfd, request, "stream unavailable"); return; }
    if (srv_data_fd < 0) { srv_job_error(respfd, request, "data lane unavailable"); return; }
    if ((uint64_t)request->ack_offset < stream->acknowledged ||
        (uint64_t)request->ack_offset > stream->offset) {
        srv_job_error(respfd, request, "ack_offset outside sent range");
        return;
    }
    uint64_t acknowledged = (uint64_t)request->ack_offset;
    uint64_t acknowledged_now = acknowledged - stream->acknowledged;
    if (acknowledged_now > stream->in_flight ||
        stream->credit > SRV_STREAM_WINDOW_MAX - (uint64_t)request->credit_bytes) {
        srv_job_error(respfd, request, "stream credit window exceeded");
        return;
    }
    uint64_t prospective_in_flight = stream->in_flight - acknowledged_now;
    uint64_t prospective_credit = stream->credit + (uint64_t)request->credit_bytes;
    if (prospective_in_flight + prospective_credit > SRV_STREAM_WINDOW_MAX) {
        srv_job_error(respfd, request, "stream credit window exceeded");
        return;
    }
    srv_job_t* job = srv_job_find(table, stream->job_id);
    if (!job) { srv_job_error(respfd, request, "stream source unavailable"); return; }
    if (srv_job_refresh(job) != 0) { srv_job_error(respfd, request, "job poll failed"); return; }
    if (srv_job_finalize_if_done(respfd, request, job) != 0) return;
    int fd = -1, terminal = 0; uint64_t size = 0; unsigned final_flags = 0;
    const char *final_ref = NULL, *final_hash = NULL;
    const char* error = srv_stream_source(job, stream->stream, 1, &fd, &size,
                                          &terminal, &final_flags,
                                          &final_ref, &final_hash);
    if (error) { srv_job_error(respfd, request, error); return; }
    if (stream->offset > size) {
        close(fd); srv_job_error(respfd, request, "stream source shrank"); return;
    }
    uint64_t sent_sequence = stream->sequence, sent_offset = stream->offset;
    uint64_t available = size - stream->offset;
    size_t wanted = available > JASH_IPC_STREAM_CHUNK_MAX
        ? JASH_IPC_STREAM_CHUNK_MAX : (size_t)available;
    if ((uint64_t)wanted > prospective_credit) wanted = (size_t)prospective_credit;
    uint64_t session_in_flight = srv_stream_in_flight() - stream->in_flight + prospective_in_flight;
    uint64_t headroom = session_in_flight < SRV_STREAM_SESSION_MAX
        ? SRV_STREAM_SESSION_MAX - session_in_flight : 0;
    if ((uint64_t)wanted > headroom) wanted = (size_t)headroom;
    unsigned char bytes[JASH_IPC_STREAM_CHUNK_MAX];
    if (wanted) {
        size_t received = 0;
        while (received < wanted) {
            ssize_t n = pread(fd, bytes + received, wanted - received,
                              (off_t)stream->offset + (off_t)received);
            if (n > 0) received += (size_t)n;
            else if (n < 0 && errno == EINTR) continue;
            else break;
        }
        if (received != wanted) {
            close(fd); srv_job_error(respfd, request, "stream spool read failed"); return;
        }
        if (jash_ipc_stream_send(srv_data_fd, stream->ref, stream->sequence,
                                 stream->offset, JASH_IPC_STREAM_DATA,
                                 bytes, wanted) != 0) {
            int saved = errno; close(fd);
            if (saved == EAGAIN || saved == EWOULDBLOCK) {
                srv_stream_result(respfd, request, stream, 0, 1, 0, 0,
                                  sent_sequence, sent_offset, 0, 0, NULL, NULL);
            } else {
                srv_job_error(respfd, request, "data lane closed");
            }
            return;
        }
        stream->acknowledged = acknowledged;
        stream->in_flight = prospective_in_flight + wanted;
        stream->credit = prospective_credit - wanted;
        stream->offset += wanted;
        stream->sequence++;
        close(fd);
        srv_stream_result(respfd, request, stream, 1, 0, 1, wanted,
                          sent_sequence, sent_offset, 0, 0, NULL, NULL);
        return;
    }
    close(fd);
    if (terminal && stream->offset == size && !stream->eof_sent) {
        if (jash_ipc_stream_send(srv_data_fd, stream->ref, stream->sequence,
                                 stream->offset, final_flags, NULL, 0) != 0) {
            int saved = errno;
            if (saved == EAGAIN || saved == EWOULDBLOCK)
                srv_stream_result(respfd, request, stream, 0, 1, 0, 0,
                                  sent_sequence, sent_offset, 0, 0, NULL, NULL);
            else srv_job_error(respfd, request, "data lane closed");
            return;
        }
        stream->acknowledged = acknowledged;
        stream->in_flight = prospective_in_flight;
        stream->credit = prospective_credit;
        stream->sequence++;
        stream->eof_sent = 1;
        int complete = !(final_flags & JASH_IPC_STREAM_INCOMPLETE);
        srv_stream_result(respfd, request, stream, 1, 0, 1, 0,
                          sent_sequence, sent_offset, 1, complete,
                          final_ref, final_hash);
        return;
    }
    stream->acknowledged = acknowledged;
    stream->in_flight = prospective_in_flight;
    stream->credit = prospective_credit;
    srv_stream_result(respfd, request, stream, 0, 0, 1, 0,
                      sent_sequence, sent_offset, stream->eof_sent,
                      stream->eof_sent && !(final_flags & JASH_IPC_STREAM_INCOMPLETE),
                      stream->eof_sent ? final_ref : NULL,
                      stream->eof_sent ? final_hash : NULL);
}

static void srv_stream_cancel(int respfd, const srv_job_req_t* request) {
    srv_data_stream_t* stream = srv_stream_find(request->stream_ref);
    if (!stream && !srv_stream_ref_shape(request->stream_ref)) {
        srv_job_error(respfd, request, "stream unavailable");
        return;
    }
    int applied = stream != NULL;
    if (stream) memset(stream, 0, sizeof *stream);
    srv_dprintf(respfd,
        "{\"schema\":\"jash.agent-stream.v1\",\"id\":%s,\"protocol_version\":2,"
        "\"op\":\"data.stream.cancel\",\"stream_ref\":\"%s\","
        "\"cancel_applied\":%s,\"ok\":true}\n",
        request->id, request->stream_ref, applied ? "true" : "false");
}

static int srv_job_cleanup(srv_job_table_t* table, const srv_receipt_signer_t* signer, const char* reason) {
    char directory[1100] = "";
    int failed = 0;
    for (int i = 0; i < SRV_JOB_MAX; i++) {
        srv_job_t* job = &table->jobs[i];
        if (!job->used) continue;
        if (!directory[0]) {
            snprintf(directory, sizeof directory, "%s", job->stdout_path);
            char* slash = strrchr(directory, '/');
            if (slash) *slash = '\0';
            else directory[0] = '\0';
        }
        int stopped = srv_job_refresh(job);
        const jash_agent_process_result_t* result = jash_agent_process_get_result(job->process);
        if (!stopped && result && result->state != JASH_AGENT_PROCESS_DONE)
            stopped = jash_agent_process_cancel(job->process, 200);
        int archived = !stopped && !srv_job_archive_terminal(job, signer, reason);
        if (!archived) {
            failed = 1;
            fprintf(stderr, "jash-agentd: session-end archive not confirmed for %s; retaining %s and %s\n", job->job_id, job->stdout_path, job->stderr_path);
        }
        jash_agent_process_destroy(job->process);
        srv_job_release_input_pin(job);
        if (job->reservation.fd >= 0) close(job->reservation.fd);
        if (archived) { unlink(job->stdout_path); unlink(job->stderr_path); }
        memset(job, 0, sizeof *job);
    }
    if (directory[0]) (void)rmdir(directory);
    return failed ? -1 : 0;
}

static int srv_data_storage_usage(uint64_t* bytes) {
    *bytes = 0; int fd = srv_store_dir("agent_artifacts");
    if (fd < 0) return errno == ENOENT ? 0 : -1;
    DIR* directory = fdopendir(fd); if (!directory) { close(fd); return -1; }
    unsigned entries = 0; int failed = 0;
    for (;;) {
        errno = 0; struct dirent* item = readdir(directory);
        if (!item) { if (errno) failed = 1; break; }
        if (!strcmp(item->d_name, ".") || !strcmp(item->d_name, "..")) continue;
        struct stat st;
        if (++entries > SRV_STORE_MAX_FILES || fstatat(fd, item->d_name, &st, AT_SYMLINK_NOFOLLOW) || !S_ISREG(st.st_mode) ||
            st.st_uid != geteuid() || (st.st_mode & 0077) || st.st_nlink != 1 || st.st_size < 0 || (uint64_t)st.st_size > UINT64_MAX - *bytes) { failed = 1; break; }
        *bytes += (uint64_t)st.st_size;
    }
    closedir(directory); return failed ? -1 : 0;
}

static int srv_data_policy_load(srv_data_policy_t* policy) {
    char text[96] = "JDP1 8388608 1\n"; size_t length = strlen(text);
    memset(policy, 0, sizeof(*policy));
    int dir = srv_store_dir("agent_store"); if (dir < 0) return -1;
    int fd = openat(dir, "data_policy", O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC); int saved = errno; close(dir);
    if (fd >= 0) {
        struct stat st; ssize_t n = -1;
        if (!fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_uid == geteuid() && !(st.st_mode & 0077) && st.st_nlink == 1 && st.st_size > 0 && st.st_size < (off_t)sizeof(text)) n = read(fd, text, sizeof(text) - 1);
        close(fd); if (n <= 0 || n != st.st_size) return -1;
        text[n] = 0; length = (size_t)n; policy->explicit_policy = 1;
    } else if (saved != ENOENT) return -1;
    if (strncmp(text, "JDP1 ", 5)) return -1;
    jp_t parser = {text + 5}; long bytes, enabled;
    if (jp_uint(&parser, SRV_DATA_MAX_BYTES, &bytes) || !bytes || *parser.p++ != ' ' || jp_uint(&parser, 1, &enabled) || strcmp(parser.p, "\n")) return -1;
    char canonical[96]; snprintf(canonical, sizeof(canonical), "JDP1 %ld %ld\n", bytes, enabled);
    if (strcmp(canonical, text)) return -1;
    unsigned char hash[32]; jash_blake3(text, length, hash); jash_blake3_hex(hash, policy->revision);
    policy->max_bytes = bytes; policy->fd_enabled = (int)enabled; return 0;
}

static void srv_data_policy_command(int respfd, const srv_job_req_t* request,
                                    const srv_receipt_signer_t* signer,
                                    srv_job_table_t* table) {
    srv_data_policy_t policy; int valid = !srv_data_policy_load(&policy), applied = 0, locked = 0;
    const char* error = valid ? NULL : "policy_invalid"; char receipt_ref[192] = {0};
    int setting = !strcmp(request->op, "data.policy.set");
    jash_json_writer_t out; jash_jsonw_init(&out, jash_jsonw_fd_sink, &respfd);
    jash_jsonw_raw(&out, "{\"schema\":\"jash.agent-data-policy.v1\",\"id\":"); jash_jsonw_string(&out, request->id_value);
    if (jash_jsonw_finish(&out)) return;
    if (setting && !error) {
        char lease_path[1100]; struct stat lease;
        snprintf(lease_path, sizeof(lease_path), "%s/.jash/agent_store/lease", srv_home());
        if (request->max_bytes < 1 || request->max_bytes > SRV_DATA_MAX_BYTES) error = "max_bytes_out_of_range";
        else if (fstat(srv_store_lease, &lease) || !srv_regular_identity_ok(lease_path, &lease)) error = "lease_identity_changed";
        else if (flock(srv_store_lease, LOCK_EX | LOCK_NB)) { error = "store_busy"; if (flock(srv_store_lease, LOCK_SH)) srv_store_poisoned = 1; }
        else locked = 1;
        if (!error && srv_data_policy_load(&policy)) { valid = 0; error = "policy_invalid"; }
        if (!error && strcmp(request->revision, policy.revision)) error = "revision_conflict";
        uint64_t durable = 0, snapshots = 0;
        if (!error && srv_data_storage_usage(&durable)) error = "artifact_measurement_incomplete";
        if (!error && srv_job_active_snapshot_bytes(table, &snapshots))
            error = "stdin_snapshot_measurement_incomplete";
        if (!error && ((uint64_t)request->max_bytes < durable ||
                       (uint64_t)request->max_bytes < srv_data_bytes() ||
                       (uint64_t)request->max_bytes < snapshots))
            error = "policy_below_usage";
        if (!error && !request->dry_run) {
            char text[96], revision[65], payload[1024], receipt_hash[65]; unsigned char hash[32];
            int length = snprintf(text, sizeof(text), "JDP1 %ld %d\n", request->max_bytes, request->fd_enabled);
            jash_blake3(text, (size_t)length, hash); jash_blake3_hex(hash, revision);
            snprintf(receipt_ref, sizeof(receipt_ref), "%s.policy-intent.dsse", request->request_digest);
            snprintf(payload, sizeof(payload), "{\"schema\":\"jash.agent-data-policy-intent.v1\",\"request_id\":%s,\"request_blake3\":\"%s\",\"prior_revision\":\"%s\",\"revision\":\"%s\"}", request->id, request->request_digest, policy.revision, revision);
            if (srv_typed_receipt_publish(signer, payload, receipt_ref, receipt_hash, "agent_data_policy_history", "application/vnd.jash.agent-data-policy-intent.v1+json")) error = "policy_intent_failed";
            if (!error) {
                int dir = srv_store_dir("agent_store"); char temp[80]; snprintf(temp, sizeof(temp), ".data-policy-%ld.tmp", (long)getpid());
                int fd = dir >= 0 ? openat(dir, temp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600) : -1;
                int good = fd >= 0 && !fchmod(fd, 0600) && !srv_write_all(fd, text, (size_t)length) && !fsync(fd);
                if (fd >= 0 && close(fd)) good = 0;
                if (good && !renameat(dir, temp, dir, "data_policy")) { applied = 1; if (fsync(dir)) error = "policy_sync_failed"; }
                else error = "policy_publish_failed";
                if (!applied && fd >= 0) (void)unlinkat(dir, temp, 0);
                if (dir >= 0) close(dir);
                if (applied) { policy.max_bytes = (uint64_t)request->max_bytes; policy.fd_enabled = request->fd_enabled; policy.explicit_policy = 1; memcpy(policy.revision, revision, sizeof(revision)); }
            }
        }
    }
    if (locked && flock(srv_store_lease, LOCK_SH)) { srv_store_poisoned = 1; error = "lease_restore_failed"; }
    jash_jsonw_raw(&out, ",\"op\":"); jash_jsonw_string(&out, request->op);
    jash_jsonw_raw(&out, ",\"revision\":"); if (valid) jash_jsonw_string(&out, policy.revision); else jash_jsonw_raw(&out, "null");
    jash_jsonw_raw(&out, ",\"max_bytes\":"); if (valid) jash_jsonw_safe_u64(&out, policy.max_bytes); else jash_jsonw_raw(&out, "null");
    jash_jsonw_raw(&out, ",\"fd_enabled\":"); jash_jsonw_raw(&out, !valid ? "null" : policy.fd_enabled ? "true" : "false");
    jash_jsonw_raw(&out, ",\"source\":"); jash_jsonw_string(&out, !valid ? "unknown" : policy.explicit_policy ? "user" : "default");
    jash_jsonw_raw(&out, ",\"authority\":\"same-owner-storage\",\"constraints\":{\"max_bytes_min\":1,\"max_bytes_max\":16777216,\"type\":\"integer\",\"fd_enabled_type\":\"boolean\"},\"applied\":"); jash_jsonw_raw(&out, applied ? "true" : "false");
    jash_jsonw_raw(&out, ",\"intent_receipt_ref\":"); if (receipt_ref[0]) jash_jsonw_string(&out, receipt_ref); else jash_jsonw_raw(&out, "null");
    jash_jsonw_raw(&out, ",\"ok\":"); jash_jsonw_raw(&out, error ? "false" : "true");
    jash_jsonw_raw(&out, ",\"error\":"); if (error) jash_jsonw_string(&out, error); else jash_jsonw_raw(&out, "null");
    jash_jsonw_raw(&out, "}\n"); (void)jash_jsonw_finish(&out);
}

static void srv_data_probe(int respfd, const srv_job_req_t* request) {
    srv_data_policy_t policy; int configured = !srv_data_policy_load(&policy);
    int compiled = jash_buffer_compiled(), available = jash_buffer_probe();
    const char* error = NULL;
    if (!configured) error = "policy_invalid";
    else if (request->transport[0] && strcmp(request->transport, "sealed-fd") && strcmp(request->transport, "inline")) error = "transport_unsupported";
    else if (!available) error = "sealed_buffers_unavailable";
    else if (!strcmp(request->transport, "sealed-fd") && !policy.fd_enabled) error = "transport_denied";
    else if (!strcmp(request->transport, "sealed-fd") && srv_data_fd < 0) error = "data_lane_unavailable";
    jash_json_writer_t out; jash_jsonw_init(&out, jash_jsonw_fd_sink, &respfd);
    jash_jsonw_raw(&out, "{\"schema\":\"jash.agent-data-probe.v1\",\"id\":"); jash_jsonw_string(&out, request->id_value);
    jash_jsonw_raw(&out, ",\"op\":\"data.probe\",\"sealed_fd\":{\"compiled\":"); jash_jsonw_raw(&out, compiled ? "true" : "false");
    jash_jsonw_raw(&out, ",\"available\":"); jash_jsonw_raw(&out, available ? "true" : "false");
    jash_jsonw_raw(&out, ",\"permitted\":"); jash_jsonw_raw(&out, configured && policy.fd_enabled ? "true" : "false");
    jash_jsonw_raw(&out, ",\"lane_enabled\":"); jash_jsonw_raw(&out, srv_data_fd >= 0 ? "true" : "false");
    jash_jsonw_raw(&out, ",\"qualification\":"); jash_jsonw_string(&out, compiled ? "linux-targeted-gates" : "unqualified");
    jash_jsonw_raw(&out, "},\"fallback\":\"explicit-only\",\"ok\":"); jash_jsonw_raw(&out, error ? "false" : "true");
    jash_jsonw_raw(&out, ",\"error\":"); if (error) jash_jsonw_string(&out, error); else jash_jsonw_raw(&out, "null");
    jash_jsonw_raw(&out, "}\n"); (void)jash_jsonw_finish(&out);
}

static srv_data_buffer_t* srv_data_find(const char* ref) {
    for (int i = 0; i < SRV_DATA_SLOTS; i++) if (srv_data_buffers[i].used && !strcmp(ref, srv_data_buffers[i].ref)) return &srv_data_buffers[i];
    return NULL;
}

static int srv_data_volatile_ref(const char* ref) {
    if (strlen(ref) != 51 || strncmp(ref, "v-", 2) || ref[34] != '-') return 0;
    for (int i = 2; i < 51; i++) if (i != 34 && !strchr("0123456789abcdef", ref[i])) return 0;
    return 1;
}

static const char* srv_data_add(int fd, uint64_t size, uint64_t limit, srv_data_buffer_t** object) {
    *object = NULL;
    if (size > limit || srv_data_bytes() > limit - size) return "buffer_quota_exceeded";
    int slot = -1; for (int i = 0; i < SRV_DATA_SLOTS; i++) if (!srv_data_buffers[i].used) { slot = i; break; }
    if (slot < 0 || srv_data_sequence == UINT64_MAX) return "buffer_slots_exhausted";
    const void* map; if (jash_buffer_map(fd, size, limit, &map)) return "buffer_not_immutable";
    unsigned char hash[32], nonce[16]; char hex[33];
    jash_blake3(map ? map : "", (size_t)size, hash); jash_buffer_unmap(map, size);
    int random = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    ssize_t n = random >= 0 ? read(random, nonce, sizeof(nonce)) : -1; if (random >= 0) close(random);
    if (n != sizeof(nonce)) return "identity_unavailable";
    srv_hex(nonce, sizeof(nonce), hex);
    srv_data_buffer_t* entry = &srv_data_buffers[slot]; memset(entry, 0, sizeof(*entry));
    entry->fd = fd; entry->size = size; entry->used = 1;
    snprintf(entry->ref, sizeof(entry->ref), "v-%s-%016llx", hex, (unsigned long long)++srv_data_sequence);
    jash_blake3_hex(hash, entry->hash); *object = entry; return NULL;
}

static const char* srv_data_publish(const srv_data_buffer_t* object, uint64_t limit, char ref[160], int* published, int* staged) {
    char directory[896], path[1100];
    snprintf(ref, 160, "a:blake3-%s-%llu.bin", object->hash, (unsigned long long)object->size);
    if (srv_private_dir("agent_artifacts", directory, sizeof(directory))) return "artifact_directory_failed";
    int n = snprintf(path, sizeof(path), "%s/%s", directory, ref + 2);
    if (n < 0 || (size_t)n >= sizeof(path)) return "artifact_path_limit";
    int lock = srv_store_lock_file("data_admission");
    if (lock < 0 || flock(lock, LOCK_EX)) { if (lock >= 0) close(lock); return "artifact_lock_failed"; }
    const char* error = NULL; struct stat exists;
    if (!lstat(path, &exists)) { if (srv_cas_verify(path, (off_t)object->size, object->hash)) error = "artifact_conflict"; }
    else if (errno != ENOENT) error = "artifact_unavailable";
    else {
        uint64_t used;
        if (srv_data_storage_usage(&used)) error = "artifact_measurement_incomplete";
        else if (used > limit || object->size > limit - used) error = "artifact_quota_exceeded";
        else {
            char temp[1100]; static unsigned serial;
            int length = snprintf(temp, sizeof(temp), "%s/.publish-%ld-%u.tmp", directory, (long)getpid(), ++serial);
            int fd = length > 0 && (size_t)length < sizeof(temp) ? open(temp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600) : -1;
            if (fd >= 0) *staged = 1;
            if (fd < 0 || fchmod(fd, 0600)) error = "artifact_stage_failed";
            uint64_t offset = 0; char bytes[65536];
            while (!error && offset < object->size) {
                size_t take = object->size - offset > sizeof(bytes) ? sizeof(bytes) : (size_t)(object->size - offset);
                ssize_t got = pread(object->fd, bytes, take, (off_t)offset);
                if (got < 0 && errno == EINTR) continue;
                if (got <= 0 || srv_write_all(fd, bytes, (size_t)got)) { error = "artifact_write_failed"; break; }
                offset += (uint64_t)got;
            }
            if (!error && fsync(fd)) error = "artifact_sync_failed";
            if (fd >= 0 && close(fd) && !error) error = "artifact_close_failed";
            if (!error && srv_cas_verify(temp, (off_t)object->size, object->hash)) error = "artifact_verify_failed";
            if (!error) {
                if (jash_osif_renameat2(AT_FDCWD, temp, AT_FDCWD, path, 1u)) error = "artifact_publish_failed";
                else { *published = 1; if (srv_fsync_dir(directory)) error = "artifact_sync_failed"; }
            }
            /* Failed staging remains accounted recovery material; do not erase
             * it merely to admit another publisher under the quota. */
        }
    }
    close(lock); return error;
}

static void srv_data_promote(int respfd, const srv_job_req_t* request, const srv_receipt_signer_t* signer, const char* text) {
    srv_idem_t reservation; int state = srv_job_idem_begin(respfd, request, text, &reservation);
    if (state) return;
    srv_data_policy_t policy; srv_data_buffer_t* object = srv_data_find(request->artifact_ref);
    const char* error = srv_data_policy_load(&policy) ? "policy_invalid" : !object ? "volatile_ref_unavailable" : NULL;
    if (error) { srv_job_idem_reject_request(respfd, &reservation, request, error); return; }
    char ref[160], receipt[192], receipt_hash[65], payload[1536], response[2048]; int published = 0, staged = 0;
    error = srv_data_publish(object, policy.max_bytes, ref, &published, &staged);
    if (error) {
        if (!staged && !published) srv_job_idem_reject_request(respfd, &reservation, request, error);
        else srv_job_idem_indeterminate(respfd, &reservation, request, error, 0, 0);
        return;
    }
    snprintf(receipt, sizeof(receipt), "%s.%s.artifact.dsse", request->request_digest, object->hash);
    int n = snprintf(payload, sizeof(payload), "{\"schema\":\"jash.agent-artifact-receipt.v1\",\"operation\":\"data.promote\",\"request_id\":%s,\"request_blake3\":\"%s\",\"artifact_ref\":\"%s\",\"content_blake3\":\"%s\",\"bytes\":%llu,\"retention\":\"keep\"}",
        request->id, request->request_digest, ref, object->hash, (unsigned long long)object->size);
    if (n < 0 || (size_t)n >= sizeof(payload) || srv_typed_receipt_publish(signer, payload, receipt, receipt_hash, "agent_artifact_receipts", "application/vnd.jash.agent-artifact-receipt.v1+json")) {
        srv_job_idem_indeterminate(respfd, &reservation, request, "artifact_receipt_failed", 0, 0); return;
    }
    n = snprintf(response, sizeof(response), "{\"schema\":\"jash.agent-artifact.v1\",\"id\":%s,\"op\":\"data.promote\",\"artifact_ref\":\"%s\",\"blake3\":\"%s\",\"bytes\":%llu,\"durability\":\"durable\",\"retention\":\"keep\",\"published\":%s,\"receipt_ref\":\"%s\",\"receipt_blake3\":\"%s\",\"receipt_pubkey\":\"%s\",\"ok\":true}\n",
        request->id, ref, object->hash, (unsigned long long)object->size, published ? "true" : "false", receipt, receipt_hash, signer->pubkey_hex);
    if (n < 0 || (size_t)n >= sizeof(response)) srv_job_idem_indeterminate(respfd, &reservation, request, "artifact_result_limit", 0, 0);
    else (void)srv_job_idem_commit(respfd, &reservation, request, response);
}

static void srv_data_command(int respfd, const srv_job_req_t* request,
                             srv_job_table_t* table) {
    jash_json_writer_t out; jash_jsonw_init(&out, jash_jsonw_fd_sink, &respfd);
    jash_jsonw_raw(&out, "{\"schema\":\"jash.agent-data.v1\",\"id\":"); jash_jsonw_string(&out, request->id_value);
    jash_jsonw_raw(&out, ",\"op\":"); jash_jsonw_string(&out, request->op);
    if (jash_jsonw_finish(&out)) return;
    srv_data_policy_t policy; const char* error = srv_data_policy_load(&policy) ? "policy_invalid" : NULL;
    if (!strcmp(request->op, "data.read") || !strcmp(request->op, "data.release")) error = NULL; /* Existing handles remain releasable. */
    srv_data_buffer_t* object = NULL; srv_data_buffer_t released = {0}; uint64_t copied = 0;
    char* encoded = NULL; uint64_t read_bytes = 0;
    if (!strcmp(request->op, "data.import")) {
        int fd = -1; uint64_t size = request->write_len;
        if (!strcmp(request->transport, "sealed-fd")) {
            if (srv_data_fd < 0) error = "data_lane_unavailable";
            else if (jash_ipc_data_receive(srv_data_fd, request->id_value, &size, &fd))
                error = errno == EAGAIN ? "attachment_missing" :
                        errno == EPIPE ? "attachment_lane_closed" : "attachment_invalid";
            else if (!error && !policy.fd_enabled) error = "transport_denied";
        } else if (!error) { fd = jash_buffer_create(request->write_data, request->write_len, policy.max_bytes); copied = request->write_len; if (fd < 0) error = "buffer_creation_failed"; }
        if (!error) error = srv_data_add(fd, size, policy.max_bytes, &object);
        if (error && fd >= 0) close(fd);
    } else if (!strcmp(request->op, "data.attach") && !error) {
        char hash[65], path[1100]; uint64_t size;
        if (strncmp(request->artifact_ref, "a:", 2) || !store_cas_name(request->artifact_ref + 2, hash, &size) || size > policy.max_bytes) error = "artifact_ref_invalid";
        else {
            int n = snprintf(path, sizeof(path), "%s/.jash/agent_artifacts/%s", srv_home(), request->artifact_ref + 2);
            struct stat st; int source = n > 0 && (size_t)n < sizeof(path) ? srv_open_regular_nofollow(path, &st) : -1;
            if (source < 0) error = "artifact_unavailable";
            else {
                int fd = jash_buffer_copy_fd(source, policy.max_bytes); close(source);
                if (fd < 0) error = "artifact_snapshot_failed";
                else {
                    const void* map;
                    if (jash_buffer_map(fd, size, policy.max_bytes, &map)) error = "artifact_size_changed";
                    else { unsigned char digest[32]; char actual[65]; jash_blake3(map ? map : "", (size_t)size, digest); jash_blake3_hex(digest, actual); jash_buffer_unmap(map, size); if (strcmp(hash, actual)) error = "artifact_hash_mismatch"; }
                    if (!error) { error = srv_data_add(fd, size, policy.max_bytes, &object); copied = size; }
                    if (error) close(fd);
                }
            }
        }
    } else if (!error) {
        if (!strcmp(request->op, "data.release"))
            for (int i = 0; i < SRV_JOB_MAX; i++)
                if (table->jobs[i].used) (void)srv_job_refresh(&table->jobs[i]);
        object = srv_data_find(request->artifact_ref);
        if (!object && !strcmp(request->op, "data.release") && srv_data_volatile_ref(request->artifact_ref)) { /* Idempotent absent release. */ }
        else if (!object) error = "volatile_ref_unavailable";
        else if (!strcmp(request->op, "data.release") && object->readers) error = "volatile_ref_in_use";
        else if (!strcmp(request->op, "data.release")) { released = *object; close(object->fd); memset(object, 0, sizeof(*object)); object = &released; }
        else if (!strcmp(request->op, "data.read")) {
            uint64_t offset = (uint64_t)request->offset;
            if (offset > object->size) error = "range_invalid";
            else {
                size_t wanted = object->size - offset < (uint64_t)request->limit ? (size_t)(object->size - offset) : (size_t)request->limit;
                unsigned char bytes[65536]; size_t got = 0;
                while (got < wanted) {
                    ssize_t n = pread(object->fd, bytes + got, wanted - got, (off_t)(offset + got));
                    if (n < 0 && errno == EINTR) continue;
                    if (n <= 0) { error = "buffer_read_failed"; break; } got += (size_t)n;
                }
                if (!error) { encoded = malloc(4 * ((got + 2) / 3) + 1); if (!encoded) error = "out_of_memory"; else { srv_b64(bytes, got, encoded); read_bytes = copied = got; } }
            }
        }
    }
    jash_jsonw_raw(&out, ",\"artifact_ref\":"); if (object) jash_jsonw_string(&out, object->ref); else jash_jsonw_raw(&out, "null");
    jash_jsonw_raw(&out, ",\"blake3\":"); if (object) jash_jsonw_string(&out, object->hash); else jash_jsonw_raw(&out, "null");
    jash_jsonw_raw(&out, ",\"bytes\":"); if (object) jash_jsonw_safe_u64(&out, object->size); else jash_jsonw_raw(&out, "null");
    jash_jsonw_raw(&out, ",\"durability\":\"volatile\",\"data_b64\":"); if (encoded) jash_jsonw_string(&out, encoded); else jash_jsonw_raw(&out, "null");
    jash_jsonw_raw(&out, ",\"read_bytes\":"); jash_jsonw_safe_u64(&out, read_bytes);
    jash_jsonw_raw(&out, ",\"copied_bytes\":"); jash_jsonw_safe_u64(&out, copied);
    jash_jsonw_raw(&out, ",\"live_buffer_bytes\":"); jash_jsonw_safe_u64(&out, srv_data_bytes());
    jash_jsonw_raw(&out, ",\"release_applied\":"); jash_jsonw_raw(&out, released.used ? "true" : "false");
    jash_jsonw_raw(&out, ",\"ok\":"); jash_jsonw_raw(&out, error ? "false" : "true");
    jash_jsonw_raw(&out, ",\"error\":"); if (error) jash_jsonw_string(&out, error); else jash_jsonw_raw(&out, "null");
    jash_jsonw_raw(&out, "}\n"); (void)jash_jsonw_finish(&out); free(encoded);
}

static void srv_budget_get(int respfd, const srv_job_req_t* request,
                           srv_job_table_t* table) {
    uint64_t cas = 0, cas_limit = 0, journal = 0, journal_limit = 0; unsigned entries = 0, jobs = 0;
    int cas_known = !srv_store_usage(&cas), cas_policy = !srv_store_limit(&cas_limit);
    int journal_known = !maintenance_journal_usage(&journal, &entries), journal_policy = !srv_store_policy_limit("journal_quota", &journal_limit);
    for (int i = 0; i < SRV_JOB_MAX; i++) if (table->jobs[i].used) jobs++;
    jash_json_writer_t out; jash_jsonw_init(&out, jash_jsonw_fd_sink, &respfd);
    jash_jsonw_raw(&out, "{\"schema\":\"jash.agent-budget.v1\",\"id\":"); jash_jsonw_string(&out, request->id_value);
    jash_jsonw_raw(&out, ",\"op\":\"budget.get\",\"observation\":\"live\",\"units\":\"logical-bytes\",\"cas_and_archives\":{\"used_bytes\":");
    if (cas_known) jash_jsonw_safe_u64(&out, cas); else jash_jsonw_raw(&out, "null");
    jash_jsonw_raw(&out, ",\"limit_bytes\":"); if (cas_policy) jash_jsonw_safe_u64(&out, cas_limit); else jash_jsonw_raw(&out, "null");
    jash_jsonw_raw(&out, "},\"journal\":{\"used_bytes\":"); if (journal_known) jash_jsonw_safe_u64(&out, journal); else jash_jsonw_raw(&out, "null");
    jash_jsonw_raw(&out, ",\"limit_bytes\":"); if (journal_policy) jash_jsonw_safe_u64(&out, journal_limit); else jash_jsonw_raw(&out, "null");
    jash_jsonw_raw(&out, ",\"required_headroom_bytes\":"); jash_jsonw_safe_u64(&out, STORE_JOURNAL_HEADROOM);
    jash_jsonw_raw(&out, "},\"job_slots_used\":"); jash_jsonw_safe_u64(&out, jobs);
    jash_jsonw_raw(&out, ",\"volatile_buffer_bytes\":"); jash_jsonw_safe_u64(&out, srv_data_bytes());
    uint64_t snapshots = 0;
    int snapshots_known = !srv_job_active_snapshot_bytes(table, &snapshots);
    jash_jsonw_raw(&out, ",\"stdin_snapshot_bytes\":");
    if (snapshots_known) jash_jsonw_safe_u64(&out, snapshots);
    else jash_jsonw_raw(&out, "null");
    jash_jsonw_raw(&out, ",\"stream_slots_used\":");
    uint64_t stream_slots = 0; for (int i = 0; i < SRV_STREAM_SLOTS; i++) stream_slots += srv_data_streams[i].used != 0;
    jash_jsonw_safe_u64(&out, stream_slots);
    jash_jsonw_raw(&out, ",\"stream_slots_max\":8,\"stream_credit_bytes\":"); jash_jsonw_safe_u64(&out, srv_stream_credit_total());
    jash_jsonw_raw(&out, ",\"stream_in_flight_bytes\":"); jash_jsonw_safe_u64(&out, srv_stream_in_flight());
    uint64_t artifacts; int measured = !srv_data_storage_usage(&artifacts);
    jash_jsonw_raw(&out, ",\"durable_artifact_bytes\":"); if (measured) jash_jsonw_safe_u64(&out, artifacts); else jash_jsonw_raw(&out, "null");
    srv_data_policy_t data_policy; int policy_known = !srv_data_policy_load(&data_policy);
    jash_jsonw_raw(&out, ",\"buffer_and_artifact_limit_bytes\":"); if (policy_known) jash_jsonw_safe_u64(&out, data_policy.max_bytes); else jash_jsonw_raw(&out, "null");
    jash_jsonw_raw(&out, ",\"job_slots_max\":32,\"memory_available_bytes\":null,\"rate_bytes_per_second\":null,\"eta_seconds\":null,\"eviction\":\"explicit-verified-only\",\"measurement_complete\":");
    jash_jsonw_raw(&out, cas_known && cas_policy && journal_known &&
                   journal_policy && measured && policy_known && snapshots_known
                   ? "true" : "false");
    jash_jsonw_raw(&out, ",\"ok\":true}\n"); (void)jash_jsonw_finish(&out);
}

static void srv_job_req_free(srv_job_req_t* request) {
    if (!request) return;
    if (request->cwd_fd >= 0) close(request->cwd_fd);
    free(request);
}

static void srv_job_dispatch(int respfd, srv_job_table_t* table,
                             const char* text,
                             const srv_receipt_signer_t* signer) {
    srv_job_req_t* request = malloc(sizeof *request);
    if (!request) {
        srv_dprintf(respfd, "{\"id\":null,\"protocol_version\":2,"
                "\"error\":\"out of memory\"}\n");
        return;
    }
    const char* error = srv_job_parse(text, request);
    if (!error) error = srv_job_validate(request);
    if (error) {
        if (srv_data_fd >= 0 && !strcmp(request->op, "data.import") && !strcmp(request->transport, "sealed-fd")) {
            uint64_t size; int fd;
            if (!jash_ipc_data_receive(srv_data_fd, request->id_value, &size, &fd)) close(fd);
        }
        srv_job_error(respfd, request, error);
        srv_job_req_free(request);
        return;
    }
    srv_job_request_digest(text, request->request_digest);
    if (!strcmp(request->op, "budget.get")) srv_budget_get(respfd, request, table);
    else if (!strcmp(request->op, "data.probe")) srv_data_probe(respfd, request);
    else if (!strncmp(request->op, "data.policy.", 12))
        srv_data_policy_command(respfd, request, signer, table);
    else if (!strcmp(request->op, "data.promote")) srv_data_promote(respfd, request, signer, text);
    else if (!strcmp(request->op, "data.stream.open")) srv_stream_open(respfd, table, request);
    else if (!strcmp(request->op, "data.stream.credit")) srv_stream_credit(respfd, table, request);
    else if (!strcmp(request->op, "data.stream.cancel")) srv_stream_cancel(respfd, request);
    else if (!strncmp(request->op, "data.", 5)) srv_data_command(respfd, request, table);
    else if (strcmp(request->op, "exec") == 0 ||
        strcmp(request->op, "job.start") == 0)
        srv_job_create(respfd, table, request, text, signer);
    else if (strcmp(request->op, "job.get") == 0 ||
             strcmp(request->op, "job.wait") == 0)
        srv_job_status(respfd, table, request, signer);
    else if (!strncmp(request->op, "store.", 6))
        srv_store_command(respfd, table, request, signer, text);
    else if (strcmp(request->op, "job.result") == 0)
        srv_job_result(respfd, request, signer);
    else if (strcmp(request->op, "job.cancel") == 0)
        srv_job_cancel(respfd, table, request, text, signer);
    else if (strcmp(request->op, "job.resize") == 0)
        srv_job_resize(respfd, table, request, text, signer);
    else if (strcmp(request->op, "job.write") == 0)
        srv_job_write(respfd, table, request, text, signer);
    else if (strcmp(request->op, "job.close-stdin") == 0)
        srv_job_close_stdin(respfd, table, request, text, signer);
    else if (strcmp(request->op, "job.read") == 0)
        srv_job_read(respfd, table, request);
    srv_job_req_free(request);
}
#endif

/* ── one request ────────────────────────────────────────────────────────── */

static void srv_handle(jash_state_t* s, const char* text, int respfd, int seq,
                       const srv_receipt_signer_t* signer) {
    srv_req_t req;
    const char* perr = srv_parse(text, &req);
    if (perr) {
        char ee[256];
        jash_json_esc(perr, ee, sizeof(ee));
        srv_dprintf(respfd, "{\"id\":%s,\"error\":\"%s\"}\n", req.id, ee);
        srv_req_free(&req);
        return;
    }

    if (req.cwd[0] && strlen(req.cwd) >= sizeof(s->cwd)) {
        srv_dprintf(respfd, "{\"id\":%s,\"error\":"
                "\"cwd exceeds runtime state limit\"}\n", req.id);
        srv_req_free(&req);
        return;
    }

#ifdef JASH_AGENT_ONLY
    const char* cap_err = srv_check_caps(s, &req);
    if (cap_err) {
        char ee[256];
        jash_json_esc(cap_err, ee, sizeof(ee));
        srv_dprintf(respfd, "{\"id\":%s,\"rc\":125,\"error\":\"%s\"}\n",
                req.id, ee);
        srv_req_free(&req);
        return;
    }

    srv_idem_t idem;
    char* replay = NULL;
    int idem_status = srv_idem_begin(&req, text, &idem, &replay);
    if (idem_status != SRV_IDEM_NEW) {
        if (idem_status == SRV_IDEM_REPLAY) {
            /* Preserve the exact signed response. Appending a replay marker
             * would invalidate result_blake3 and make the receipt unverifiable. */
            if (req.request_version == 2)
                srv_dprintf(respfd, "{\"id\":%s,\"event\":\"start\","
                        "\"protocol_version\":2,\"replayed\":true}\n",
                        req.id);
            if (strstr(replay, ",\"cwd\":\"") &&
                strstr(replay, ",\"cwd_bytes_hex\":\""))
                (void)srv_write_all(respfd, replay, strlen(replay));
            else srv_dprintf(respfd, "{\"id\":%s,\"error\":\"bad stored response\","
                         "\"outcome\":\"indeterminate\"}\n", req.id);
            free(replay);
        } else if (idem_status == SRV_IDEM_CONFLICT) {
            srv_dprintf(respfd, "{\"id\":%s,\"rc\":125,"
                    "\"error\":\"idempotency conflict\"}\n", req.id);
        } else {
            srv_dprintf(respfd, "{\"id\":%s,\"rc\":125,"
                    "\"error\":\"request outcome unknown\","
                    "\"outcome\":\"indeterminate\"}\n", req.id);
        }
        srv_req_free(&req);
        return;
    }
#endif

    /* Per-request env overlay: save, set, run, restore. */
    char* saved_v[SRV_ENV_MAX] = {0};
    int   saved_had[SRV_ENV_MAX];
    int   cwd_changed = 0;
    int   env_save_failed = 0;
    for (int i = 0; i < req.env_n; i++) {
        const char* old = jash_env_get(s, req.env_k[i]);
        saved_had[i] = old != NULL;
        saved_v[i] = old ? strdup(old) : NULL;
        if (old && !saved_v[i]) { env_save_failed = 1; break; }
    }
    /* Never execute a partial overlay, including when the env table is full. */
    if (env_save_failed || (req.env_n &&
        jash_env_apply_batch(s, (const char* const*)req.env_k,
                             (const char* const*)req.env_v, req.env_n) != 0)) {
        char response[JASH_AGENT_ID_JSON_CAP + 256];
        snprintf(response, sizeof response,
                 "{\"id\":%s,\"error\":\"env overlay setup failed\"}\n", req.id);
#ifdef JASH_AGENT_ONLY
        srv_idem_reject(respfd, &idem, req.id, response);
#else
        srv_dprintf(respfd, "%s", response);
#endif
        for (int i = 0; i < req.env_n; i++) free(saved_v[i]);
        srv_req_free(&req);
        return;
    }

    /* Build the command line. */
    char* cmd = (char*)malloc(SRV_LINE_MAX * 5);
    int build_err = 0;
    if (!cmd) build_err = 1;
    else if (req.argc > 0)
        build_err = srv_argv_to_line(&req, cmd, SRV_LINE_MAX * 5) != 0;
    else
        snprintf(cmd, SRV_LINE_MAX * 5, "%s", req.line);

    if (build_err) {
#ifdef JASH_AGENT_ONLY
        char response[JASH_AGENT_ID_JSON_CAP + 256];
        snprintf(response, sizeof(response),
                 "{\"id\":%s,\"error\":\"command too long\"}\n", req.id);
        srv_idem_reject(respfd, &idem, req.id, response);
#else
        srv_dprintf(respfd, "{\"id\":%s,\"error\":\"command too long\"}\n", req.id);
#endif
        free(cmd);
        goto restore_env;
    }

#ifdef JASH_AGENT_ONLY
    if (req.timeout_ms >= 0) {
        char* wrapped = (char*)malloc(SRV_LINE_MAX * 5);
        if (!wrapped) {
            char response[JASH_AGENT_ID_JSON_CAP + 256];
            snprintf(response, sizeof(response),
                     "{\"id\":%s,\"error\":\"oom\"}\n", req.id);
            srv_idem_reject(respfd, &idem, req.id, response);
            free(cmd);
            goto restore_env;
        }
        int n = snprintf(wrapped, SRV_LINE_MAX * 5,
                         "timeout -k 0.2 %.3f %s",
                         (double)req.timeout_ms / 1000.0, cmd);
        free(cmd);
        cmd = wrapped;
        if (n <= 0 || n >= SRV_LINE_MAX * 5) {
            char response[JASH_AGENT_ID_JSON_CAP + 256];
            snprintf(response, sizeof(response),
                     "{\"id\":%s,\"error\":\"command too long\"}\n", req.id);
            srv_idem_reject(respfd, &idem, req.id, response);
            free(cmd);
            goto restore_env;
        }
    }
#endif

    /* Delay sticky cwd mutation until every fallible preparation step has
     * succeeded; pre-execution rejection must remain effect-free. */
    if (req.cwd[0]) {
        char* relative_cwd = NULL;
        const char* state_cwd = req.cwd;
        if (req.cwd[0] != '/') {
            relative_cwd = realpath(req.cwd, NULL);
            if (!relative_cwd || strlen(relative_cwd) >= sizeof(s->cwd)) {
#ifdef JASH_AGENT_ONLY
                char response[JASH_AGENT_ID_JSON_CAP + 256];
                snprintf(response, sizeof(response),
                         "{\"id\":%s,\"error\":\"relative cwd is not representable\"}\n",
                         req.id);
                srv_idem_reject(respfd, &idem, req.id, response);
#else
                srv_dprintf(respfd, "{\"id\":%s,\"error\":"
                        "\"relative cwd is not representable\"}\n", req.id);
#endif
                free(relative_cwd);
                free(cmd);
                goto restore_env;
            }
            state_cwd = relative_cwd;
        }
        if (chdir(req.cwd) != 0) {
#ifdef JASH_AGENT_ONLY
            char response[JASH_AGENT_ID_JSON_CAP + 256];
            snprintf(response, sizeof(response),
                     "{\"id\":%s,\"error\":\"chdir failed\"}\n", req.id);
            srv_idem_reject(respfd, &idem, req.id, response);
#else
            srv_dprintf(respfd, "{\"id\":%s,\"error\":\"chdir failed\"}\n", req.id);
#endif
            free(relative_cwd);
            free(cmd);
            goto restore_env;
        }
        snprintf(s->cwd, sizeof(s->cwd), "%s", state_cwd);
        jash_env_set(s, "PWD", state_cwd);
        free(relative_cwd);
        cwd_changed = 1;
    }

#ifdef JASH_AGENT_ONLY
    int state_cwd_flags = O_RDONLY;
#ifdef O_CLOEXEC
    state_cwd_flags |= O_CLOEXEC;
#endif
    int state_cwd_fd = open(".", state_cwd_flags);
    char state_cwd_before[JASH_CWD_MAX];
    snprintf(state_cwd_before, sizeof state_cwd_before, "%s", s->cwd);
    if (state_cwd_fd < 0) {
        char response[JASH_AGENT_ID_JSON_CAP + 256];
        snprintf(response, sizeof response,
                 "{\"id\":%s,\"error\":\"cwd checkpoint failed\"}\n", req.id);
        srv_idem_reject(respfd, &idem, req.id, response);
        free(cmd);
        goto restore_env;
    }
    int cwd_state_failed = 0;
    int cwd_restore_failed = 0;
#endif

    /* Structured argv already has a reviewed, fail-closed effect. Do not turn
     * a read builtin back into quoted shell text and then misclassify it as an
     * unknown command: that made every agent `echo` scan the full cwd. */
    int request_is_read_only = req.argc > 0 &&
        strcmp(jash_builtin_effect_argv(req.argv[0], req.argc - 1, req.argv + 1), "read") == 0;

    /* W2 blast radius, per request. */
    jash_fssnap_t* snap = NULL;
    char root[1024] = "";
    if (!request_is_read_only &&
        !jash_fschange_command_is_pure(cmd) && getcwd(root, sizeof(root)))
        snap = jash_fssnap_take(root);

    /* Capture files + optional stdin file. */
    char outp[256], errp[256], inp[256];
    jash_tmp_path(".serve_out", outp, sizeof(outp));
    jash_tmp_path(".serve_err", errp, sizeof(errp));
    int ofd = srv_cloexec(open(outp, O_RDWR | O_CREAT | O_TRUNC, 0600));
    int efd = srv_cloexec(open(errp, O_RDWR | O_CREAT | O_TRUNC, 0600));
    int ifd = -1;
    int stdin_setup_failed = 0;
    int streaming_v2 = 0;
#ifdef JASH_AGENT_ONLY
    streaming_v2 = req.request_version == 2;
#endif
    if (req.stdin_buf && !streaming_v2) {
        jash_tmp_path(".serve_in", inp, sizeof(inp));
        ifd = srv_cloexec(open(inp, O_RDWR | O_CREAT | O_TRUNC, 0600));
        if (ifd >= 0) {
            size_t off = 0;
            while (off < req.stdin_len) {
                ssize_t written = write(ifd, req.stdin_buf + off,
                                        req.stdin_len - off);
                if (written > 0) off += (size_t)written;
                else if (written < 0 && errno == EINTR) continue;
                else { stdin_setup_failed = 1; break; }
            }
            if (!stdin_setup_failed && lseek(ifd, 0, SEEK_SET) < 0)
                stdin_setup_failed = 1;
        } else stdin_setup_failed = 1;
    }

#ifdef JASH_AGENT_ONLY
    if (ofd < 0 || efd < 0 || stdin_setup_failed) {
        if (ofd >= 0) close(ofd);
        if (efd >= 0) close(efd);
        if (ifd >= 0) close(ifd);
        close(state_cwd_fd);
        unlink(outp);
        unlink(errp);
        if (req.stdin_buf && !streaming_v2) unlink(inp);
        char response[JASH_AGENT_ID_JSON_CAP + 256];
        snprintf(response, sizeof response,
                 "{\"id\":%s,\"error\":\"capture setup failed\"}\n",
                 req.id);
        srv_idem_reject(respfd, &idem, req.id, response);
        free(cmd);
        goto restore_env;
    }
#else
    if (ofd < 0 || efd < 0 || stdin_setup_failed) {
        if (ofd >= 0) close(ofd);
        if (efd >= 0) close(efd);
        if (ifd >= 0) close(ifd);
        unlink(outp);
        unlink(errp);
        if (req.stdin_buf) unlink(inp);
        srv_dprintf(respfd, "{\"id\":%s,\"error\":\"capture setup failed\"}\n",
                req.id);
        free(cmd);
        goto restore_env;
    }
#endif

#ifdef JASH_AGENT_ONLY
    int stdout_pipe[2] = {-1, -1};
    int stderr_pipe[2] = {-1, -1};
    int stdin_pipe[2] = {-1, -1};
    pid_t pump_pid = -1;
    int pump_failed = 0;
    if (streaming_v2 && ofd >= 0 && efd >= 0 &&
        pipe(stdout_pipe) == 0 && pipe(stderr_pipe) == 0 &&
        pipe(stdin_pipe) == 0) {
        pump_pid = fork();
        if (pump_pid == 0) {
            /* Only the evaluator owns cancellation. CLOEXEC alone would leave
             * this fork-only IO helper holding the endpoint until it exits. */
            if (s->agent_cancel_fd >= 0) close(s->agent_cancel_fd);
            if (srv_shutdown_fd >= 0) close(srv_shutdown_fd);
            if (srv_store_lease >= 0) close(srv_store_lease);
            srv_data_close_child();
            close(stdout_pipe[1]);
            close(stderr_pipe[1]);
            close(stdin_pipe[0]);
            int pump_rc = srv_pump_io(
                respfd, stdout_pipe[0], stderr_pipe[0], stdin_pipe[1],
                ofd, efd, req.id, (const uint8_t*)req.stdin_buf,
                req.stdin_buf ? req.stdin_len : 0);
            close(ofd);
            close(efd);
            _exit(pump_rc == 0 ? 0 : 1);
        }
    }
    if (streaming_v2 && pump_pid < 0) {
        for (int i = 0; i < 2; i++) {
            if (stdout_pipe[i] >= 0) close(stdout_pipe[i]);
            if (stderr_pipe[i] >= 0) close(stderr_pipe[i]);
            if (stdin_pipe[i] >= 0) close(stdin_pipe[i]);
        }
        if (ofd >= 0) close(ofd);
        if (efd >= 0) close(efd);
        close(state_cwd_fd);
        unlink(outp);
        unlink(errp);
        char response[JASH_AGENT_ID_JSON_CAP + 256];
        snprintf(response, sizeof response,
                 "{\"id\":%s,\"error\":\"stream pump setup failed\"}\n",
                 req.id);
        srv_idem_reject(respfd, &idem, req.id, response);
        free(cmd);
        goto restore_env;
    }
#endif

    int rc = 127;
    double ms = 0;
#ifdef JASH_AGENT_ONLY
    int native_cancel = (req.cancelable || srv_shutdown_fd >= 0) &&
        (jash_mutation_cancelable(req.argv[0], req.argc - 1, req.argv + 1) ||
         jash_query_cancelable(req.argv[0], req.argc - 1, req.argv + 1));
    if (native_cancel) {
        jash_mutation_cancel_control(srv_native_cancel, s);
        jash_query_cancel_control(srv_native_cancel, s);
    }
    s->agent_request_cancelled = 0;
    s->agent_request_timed_out = 0;
    s->agent_request_kill_incomplete = 0;
    if (req.cancelable) {
        srv_cancel_discard_stale(s);
        snprintf(s->agent_active_request_id, sizeof(s->agent_active_request_id),
                 "%s", req.id_value);
        if (!streaming_v2)
            srv_dprintf(respfd, "{\"id\":%s,\"active\":true,"
                    "\"cancelable\":true}\n", req.id);
    }
    if (streaming_v2)
        srv_dprintf(respfd, "{\"id\":%s,\"event\":\"start\","
                "\"protocol_version\":2,\"active\":true,"
                "\"cancelable\":%s}\n", req.id,
                req.cancelable ? "true" : "false");
#endif
    if (streaming_v2) {
#ifdef JASH_AGENT_ONLY
        close(stdout_pipe[0]);
        close(stderr_pipe[0]);
        close(stdin_pipe[1]);
        int so = srv_dup_cloexec(1), se = srv_dup_cloexec(2);
        int si = srv_dup_cloexec(0);
        dup2(stdout_pipe[1], 1);
        dup2(stderr_pipe[1], 2);
        dup2(stdin_pipe[0], 0);
        close(stdout_pipe[1]);
        close(stderr_pipe[1]);
        close(stdin_pipe[0]);
        jash_stdin_stream_sync(1);

        struct timeval t0, t1;
        gettimeofday(&t0, NULL);
        rc = srv_response_failed ? 125 : jash_eval_line(s, srv_fd1_writer, 0, cmd);
        gettimeofday(&t1, NULL);
        ms = (double)(t1.tv_sec - t0.tv_sec) * 1000.0 +
             (double)(t1.tv_usec - t0.tv_usec) / 1000.0;

        dup2(so, 1); dup2(se, 2); dup2(si, 0);
        jash_stdin_stream_sync(1);
        close(so); close(se); close(si);
        int pump_status = 0;
        pid_t waited;
        do { waited = waitpid(pump_pid, &pump_status, 0); }
        while (waited < 0 && errno == EINTR);
        pump_failed = waited != pump_pid || !WIFEXITED(pump_status) ||
                      WEXITSTATUS(pump_status) != 0;
#endif
    } else if (ofd >= 0 && efd >= 0) {
        int so = srv_dup_cloexec(1), se = srv_dup_cloexec(2);
        int si = srv_dup_cloexec(0);
        dup2(ofd, 1);
        dup2(efd, 2);
        if (ifd >= 0) {
            dup2(ifd, 0);
            jash_stdin_stream_sync(1);
        }
        else {
            int nul = srv_cloexec(open("/dev/null", O_RDONLY));
            if (nul >= 0) {
                dup2(nul, 0);
                jash_stdin_stream_sync(1);
                close(nul);
            }
        }

        struct timeval t0, t1;
        gettimeofday(&t0, NULL);
        rc = srv_response_failed ? 125 : jash_eval_line(s, srv_fd1_writer, 0, cmd);
        gettimeofday(&t1, NULL);
        ms = (double)(t1.tv_sec - t0.tv_sec) * 1000.0 +
             (double)(t1.tv_usec - t0.tv_usec) / 1000.0;

        dup2(so, 1); dup2(se, 2); dup2(si, 0);
        jash_stdin_stream_sync(1);
        close(so); close(se); close(si);
    }
#ifdef JASH_AGENT_ONLY
    s->agent_active_request_id[0] = '\0';
    if (native_cancel) {
        jash_mutation_cancel_control(NULL, NULL);
        jash_query_cancel_control(NULL, NULL);
        if (rc == 130 || rc == 143) s->agent_request_cancelled = 1;
    }
    struct stat actual_cwd_st, state_cwd_st;
    int state_matches = s->cwd[0] && stat(".", &actual_cwd_st) == 0 &&
        stat(s->cwd, &state_cwd_st) == 0 &&
        actual_cwd_st.st_dev == state_cwd_st.st_dev &&
        actual_cwd_st.st_ino == state_cwd_st.st_ino;
    char recovered_cwd[JASH_CWD_MAX];
    if (!state_matches && getcwd(recovered_cwd, sizeof recovered_cwd)) {
        snprintf(s->cwd, sizeof(s->cwd), "%s", recovered_cwd);
        jash_env_set(s, "PWD", s->cwd);
    } else if (!state_matches) {
        cwd_state_failed = 1;
        rc = 125;
        if (fchdir(state_cwd_fd) != 0) {
            cwd_restore_failed = 1;
        } else {
            struct stat restored_st, prior_st;
            int prior_matches = stat(".", &restored_st) == 0 &&
                stat(state_cwd_before, &prior_st) == 0 &&
                restored_st.st_dev == prior_st.st_dev &&
                restored_st.st_ino == prior_st.st_ino;
            if (prior_matches) {
                snprintf(s->cwd, sizeof(s->cwd), "%s", state_cwd_before);
            } else if (getcwd(recovered_cwd, sizeof recovered_cwd)) {
                snprintf(s->cwd, sizeof(s->cwd), "%s", recovered_cwd);
            } else {
                cwd_restore_failed = 1;
            }
            if (!cwd_restore_failed) jash_env_set(s, "PWD", s->cwd);
        }
    } else jash_env_set(s, "PWD", s->cwd);
    close(state_cwd_fd);
#endif
    if (ofd >= 0) close(ofd);
    if (efd >= 0) close(efd);
    if (ifd >= 0) { close(ifd); unlink(inp); }

    /* files_changed. */
    jash_fsdiff_t fsd;
    memset(&fsd, 0, sizeof(fsd));
    if (snap) {
        jash_fssnap_diff(snap, root, &fsd);
        jash_fsdiff_persist(&fsd);
    }

#ifdef JASH_AGENT_ONLY
    if (cwd_restore_failed) {
        s->exit_requested = 1;
        s->last_status.code = 125;
        srv_dprintf(respfd, "{\"id\":%s,\"error\":"
                "\"cwd rollback failed\",\"outcome\":\"indeterminate\"}\n",
                req.id);
        unlink(outp);
        unlink(errp);
        free(cmd);
        goto restore_env;
    }
    if (s->agent_request_kill_incomplete) {
        srv_dprintf(respfd, "{\"id\":%s,\"error\":"
                "\"process group termination unconfirmed\","
                "\"outcome\":\"indeterminate\"}\n", req.id);
        unlink(outp);
        unlink(errp);
        free(cmd);
        goto restore_env;
    }
    if (pump_failed) {
        srv_dprintf(respfd, "{\"id\":%s,\"error\":"
                "\"stream pump failed\",\"outcome\":\"indeterminate\"}\n",
                req.id);
        unlink(outp);
        unlink(errp);
        free(cmd);
        goto restore_env;
    }
#endif

    /* Response. */
    size_t rcap = SRV_RESPONSE_MAX;
    char* resp = (char*)malloc(rcap);
    if (resp) {
#ifdef JASH_AGENT_ONLY
        char cwd_escaped[JASH_CWD_MAX * 6 + 1];
        char cwd_hex[JASH_CWD_MAX * 2 + 1];
        srv_json_bytes_esc(s->cwd, cwd_escaped, sizeof cwd_escaped);
        srv_hex((const uint8_t*)s->cwd, strlen(s->cwd), cwd_hex);
        if (streaming_v2)
            snprintf(resp, rcap,
                     "{\"id\":%s,\"event\":\"result\","
                     "\"protocol_version\":2,\"rc\":%d,"
                     "\"duration_ms\":%.0f,\"cwd\":\"%s\","
                     "\"cwd_bytes_hex\":\"%s\"",
                     req.id, rc, ms, cwd_escaped, cwd_hex);
        else
            snprintf(resp, rcap,
                     "{\"id\":%s,\"rc\":%d,\"duration_ms\":%.0f,"
                     "\"cwd\":\"%s\",\"cwd_bytes_hex\":\"%s\"",
                     req.id, rc, ms, cwd_escaped, cwd_hex);
        if (req.timeout_ms >= 0) {
            size_t o = strlen(resp);
            snprintf(resp + o, rcap - o, ",\"timed_out\":%s",
                     s->agent_request_timed_out ? "true" : "false");
        }
        if (req.cancelable || native_cancel) {
            size_t o = strlen(resp);
            snprintf(resp + o, rcap - o, ",\"cancelled\":%s",
                     s->agent_request_cancelled ? "true" : "false");
        }
        if (cwd_state_failed) {
            size_t o = strlen(resp);
            snprintf(resp + o, rcap - o,
                     ",\"error\":\"cwd exceeds runtime state limit\","
                     "\"outcome\":\"indeterminate\"");
        }
#else
        snprintf(resp, rcap, "{\"id\":%s,\"rc\":%d,\"duration_ms\":%.0f",
                 req.id, rc, ms);
#endif
        /* ADR-0038 W7: flag instruction-shaped text in stdout BEFORE the
         * stream field moves/consumes the file. Structured, not a warning
         * line — the protocol stream stays machine-clean. */
        {
            const char* pat = NULL; long off = 0;
            if (jash_prov_enabled() &&
                jash_prov_scan_file(outp, 256 * 1024, &pat, &off)) {
                char ep[160];
                jash_json_esc(pat, ep, sizeof(ep));
                size_t o = strlen(resp);
                snprintf(resp + o, rcap - o,
                    ",\"provenance\":{\"instruction_shaped\":true,"
                    "\"pattern\":\"%s\",\"offset\":%ld}", ep, off);
            }
        }
        char stdout_hash[JASH_BLAKE3_HEX_LEN] = "";
        char stderr_hash[JASH_BLAKE3_HEX_LEN] = "";
        int stream_meta_failed = 0;
#ifdef JASH_AGENT_ONLY
        if (streaming_v2) {
            stream_meta_failed =
                srv_stream_v2_json("stdout", outp, resp, rcap,
                                   stdout_hash) != 0 ||
                srv_stream_v2_json("stderr", errp, resp, rcap,
                                   stderr_hash) != 0;
        } else
#endif
        {
            stream_meta_failed =
                srv_stream_json("stdout", outp, seq, resp, rcap, stdout_hash) != 0 ||
                srv_stream_json("stderr", errp, seq, resp, rcap, stderr_hash) != 0;
        }
#ifndef JASH_AGENT_ONLY
        if (stream_meta_failed)
            snprintf(resp, rcap,
                     "{\"id\":%s,\"rc\":125,\"command_status\":%d,"
                     "\"complete\":false,\"error\":\"capture_failed\"",
                     req.id, rc);
#endif
        jash_json_buffer_t metadata_buffer = {resp, rcap, strlen(resp)};
        jash_json_writer_t metadata;
        jash_jsonw_init(&metadata, jash_jsonw_buffer_sink, &metadata_buffer);
        if (fsd.valid) {
            jash_jsonw_raw(&metadata, ",\"files_changed\":{\"total\":");
            jash_jsonw_i64(&metadata, fsd.total);
            const char* kinds = "+~-";
            const char* names[3] = {"created", "modified", "deleted"};
            for (int k = 0; k < 3; k++) {
                jash_jsonw_raw(&metadata, ","); jash_jsonw_string(&metadata, names[k]); jash_jsonw_raw(&metadata, ":[");
                int first = 1;
                for (int i = 0; i < fsd.n; i++) {
                    if (fsd.kind[i] != kinds[k]) continue;
                    if (!first) jash_jsonw_raw(&metadata, ",");
                    first = 0; jash_jsonw_string_or_bytes(&metadata, fsd.paths[i]);
                }
                jash_jsonw_raw(&metadata, "]");
            }
            jash_jsonw_raw(&metadata, ",\"stale_writes\":[");
            int first = 1;
            for (int i = 0; i < fsd.n; i++) if (fsd.stale[i]) {
                if (!first) jash_jsonw_raw(&metadata, ",");
                first = 0; jash_jsonw_string_or_bytes(&metadata, fsd.paths[i]);
            }
            jash_jsonw_raw(&metadata, "]}");
        }
        jash_jsonw_raw(&metadata, "}\n");
        if (jash_jsonw_finish(&metadata)) {
            stream_meta_failed = 1;
            snprintf(resp, rcap, "{\"id\":%s,\"rc\":125,\"command_status\":%d,\"complete\":false,\"error\":\"response_limit_exceeded\"}\n", req.id, rc);
        }
#ifdef JASH_AGENT_ONLY
        char receipt_ref[160], receipt_hash[JASH_BLAKE3_HEX_LEN];
        if (stream_meta_failed || !stdout_hash[0] || !stderr_hash[0] ||
            srv_receipt_publish(signer, &req, &idem, resp,
                                stdout_hash, stderr_hash, s->cwd, rc, ms,
                                s->agent_request_timed_out,
                                s->agent_request_cancelled, receipt_ref,
                                sizeof receipt_ref, receipt_hash) != 0) {
            srv_dprintf(respfd, "{\"id\":%s,\"error\":\"receipt commit failed\","
                    "\"outcome\":\"indeterminate\"}\n", req.id);
            free(resp);
            resp = NULL;
        } else {
            size_t n = strlen(resp);
            if (n >= 2) resp[n - 2] = '\0';
            snprintf(resp + strlen(resp), rcap - strlen(resp),
                     ",\"receipt_ref\":\"%s\",\"receipt_blake3\":\"%s\","
                     "\"receipt_pubkey\":\"%s\"}\n",
                     receipt_ref, receipt_hash, signer->pubkey_hex);
        }
        if (resp)
        srv_idem_finish(respfd, &idem, req.id, resp);
#else
        (void)srv_write_all(respfd, resp, strlen(resp));
#endif
        free(resp);
    }
    unlink(outp);
    unlink(errp);
    free(cmd);

restore_env:
#ifdef JASH_AGENT_ONLY
    if (idem.fd >= 0) {
        close(idem.fd);
        idem.fd = -1;
    }
#endif
    for (int i = 0; i < req.env_n; i++) {
        if (saved_had[i]) jash_env_set(s, req.env_k[i], saved_v[i]);
        else jash_env_unset(s, req.env_k[i]);
        free(saved_v[i]);
    }
    if (cwd_changed) jash_env_set(s, "PWD", s->cwd);
    srv_req_free(&req);
}

/* ── the loop ───────────────────────────────────────────────────────────── */

static int srv_serve_loop(jash_state_t* s) {
    int reqfd = srv_dup_cloexec(0);
    int respfd = srv_dup_cloexec(1);
    if (reqfd < 0 || respfd < 0) { if (reqfd >= 0) close(reqfd); if (respfd >= 0) close(respfd); return 1; }
    srv_response_fd = respfd; srv_response_failed = 0;
    FILE* in = fdopen(reqfd, "r");
    if (!in) return 1;

    srv_receipt_signer_t signer;
    memset(&signer, 0, sizeof signer);
#ifdef JASH_AGENT_ONLY
    srv_job_table_t job_table;
    memset(&job_table, 0, sizeof job_table);
    job_table.cgroup_parent_fd = -1;
    char runtime_cwd[JASH_CWD_MAX];
    if (!getcwd(runtime_cwd, sizeof runtime_cwd)) {
        srv_dprintf(respfd, "{\"runtime\":\"%s\",\"error\":"
                "\"initial cwd exceeds runtime state limit\"}\n",
                JASH_AGENT_RUNTIME_NAME);
        fclose(in);
        close(respfd);
        return 125;
    }
    snprintf(s->cwd, sizeof(s->cwd), "%s", runtime_cwd);
    jash_env_set(s, "PWD", s->cwd);
    if (srv_receipt_signer_load(&signer) != 0) {
        srv_dprintf(respfd, "{\"runtime\":\"%s\",\"error\":"
                "\"receipt signing key unavailable\"}\n",
                JASH_AGENT_RUNTIME_NAME);
        fclose(in);
        close(respfd);
        return 125;
    }
    const char* cancel_env = getenv("JASH_AGENT_CANCEL_FD");
    if (cancel_env && *cancel_env) {
        char* end = NULL;
        long fd = strtol(cancel_env, &end, 10);
        if (!end || *end || fd < 3 || fcntl((int)fd, F_GETFD) < 0) {
            srv_dprintf(respfd, "{\"runtime\":\"%s\",\"error\":"
                    "\"invalid JASH_AGENT_CANCEL_FD\"}\n",
                    JASH_AGENT_RUNTIME_NAME);
            fclose(in);
            close(respfd);
            return 125;
        }
        s->agent_cancel_fd = (int)fd;
        (void)fcntl(s->agent_cancel_fd, F_SETFD,
                    fcntl(s->agent_cancel_fd, F_GETFD) | FD_CLOEXEC);
        int flags = fcntl(s->agent_cancel_fd, F_GETFL, 0);
        if (flags >= 0) (void)fcntl(s->agent_cancel_fd, F_SETFL, flags | O_NONBLOCK);
    }
    const char* cgroup_root = getenv("JASH_AGENT_CGROUP_ROOT");
    if (cgroup_root && *cgroup_root) {
        job_table.cgroup_parent_fd = cgroup_root[0] == '/'
            ? open(cgroup_root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC) : -1;
        if (job_table.cgroup_parent_fd < 0) {
            srv_dprintf(respfd, "{\"runtime\":\"%s\",\"error\":\"invalid JASH_AGENT_CGROUP_ROOT\"}\n",
                    JASH_AGENT_RUNTIME_NAME);
            fclose(in);
            close(respfd);
            return 125;
        }
    }
    srv_dprintf(respfd,
            "{\"runtime\":\"%s\",\"profile\":\"agent-strict\","
            "\"version\":\"%s\",\"pid\":%d,\"protocol\":\"jsonl\","
            "\"protocol_version\":%d,\"schema_version\":%d,"
            "\"protocol_versions\":[1,2],\"request_version_default\":1,"
            "\"build\":{\"git\":\"%s\",\"git_available\":%s,"
            "\"codename\":\"%s\",\"modabi\":%d},"
            "\"fields\":[\"id\",\"op\",\"ir\",\"job_id\",\"argv\","
            "\"data_b64\",\"artifact_ref\",\"transport\",\"revision\",\"fd_enabled\","
            "\"background\",\"pty\",\"rows\",\"cols\",\"wait_ms\","
            "\"grace_ms\",\"stream\",\"offset\",\"limit\",\"line\",\"stdin\","
            "\"stdin_b64\",\"stdin_mode\",\"request_version\",\"protocol_version\","
            "\"cwd\",\"execution_spec_sha256\",\"resources\",\"env\",\"caps\","
            "\"timeout_ms\",\"cancelable\",\"max_bytes\",\"older_than_seconds\",\"dry_run\",\"target_id\",\"cursor\",\"root_namespace\",\"root_name\",\"root_blake3\","
            "\"stream_ref\",\"credit_bytes\",\"ack_offset\"],"
            "\"response_fields\":[\"id\",\"rc\",\"duration_ms\",\"cwd\","
            "\"cwd_bytes_hex\",\"event\",\"protocol_version\","
            "\"timed_out\",\"cancelled\",\"stdout\",\"stderr\","
            "\"stdout_ref\",\"stderr_ref\",\"files_changed\","
            "\"receipt_ref\",\"receipt_blake3\",\"receipt_pubkey\","
            "\"stdout_bytes\",\"stderr_bytes\",\"stdout_blake3\","
            "\"stderr_blake3\",\"stream\",\"seq\",\"data_b64\","
            "\"stdout_head\","
            "\"stderr_head\",\"provenance\",\"cancelable\",\"op\","
            "\"job_id\",\"state\",\"terminal\",\"accepted\",\"data_b64\","
            "\"offset\",\"next_offset\",\"bytes\",\"accepted_bytes\",\"eof\","
            "\"stream_ref\",\"credit_bytes\",\"ack_offset\",\"next_sequence\","
            "\"packet_sent\",\"blocked\",\"credit_applied\",\"complete\","
            "\"credit_available_bytes\",\"in_flight_bytes\",\"final_ref\","
            "\"final_blake3\",\"cancel_applied\",\"source_size_bytes\","
            "\"source_terminal\",\"delivery\",\"close_applied\","
            "\"quiescent\",\"quiescence_forced\",\"output_limit\","
            "\"receipt_phase\",\"start_receipt_ref\","
            "\"start_receipt_blake3\",\"start_request_blake3\","
            "\"resource_backend\",\"resource_enforced\",\"resources\","
            "\"resource_violation\",\"resource_timed_out\",\"resource_empty\",\"descendants_forced\","
            "\"resource_memory_events\",\"resource_pids_events\","
            "\"stdin_artifact_ref\",\"stdin_blake3\",\"stdin_bytes\","
            "\"stdin_stream_ref\",\"stdin_stream_offset\","
            "\"stdin_stream_final_ref\",\"stdin_stream_final_blake3\","
            "\"stdin_durability\",\"stdin_delivery\",\"stdin_serialized_bytes\","
            "\"stdin_materialized_bytes\",\"stdin_verified_bytes\","
            "\"error\",\"outcome\",\"active\"],"
            "\"inventory\":{\"builtins\":%d,\"effects_classified\":%d},"
            "\"effect_map\":{",
            JASH_AGENT_RUNTIME_NAME, JASH_VERSION, (int)getpid(),
            JASH_AGENT_PROTOCOL_VERSION, JASH_AGENT_SCHEMA_VERSION,
            JASH_GITHASH,
            (strcmp(JASH_GITHASH, "unknown") != 0 &&
             strcmp(JASH_GITHASH, "nogit") != 0) ? "true" : "false",
            JASH_CODENAME, JMOD_ABI_VERSION,
            jash_builtin_count(), jash_builtin_effect_classified());
    for (int i = 0; i < jash_builtin_count(); i++) {
        const jash_builtin_t* b = jash_builtin_at((uint32_t)i);
        char name[256];
        jash_json_esc(b->name, name, sizeof name);
        srv_dprintf(respfd, "%s\"%s\":\"%s\"", i ? "," : "", name,
                jash_builtin_effect(b->name));
    }
#ifdef __linux__
    const char* pty_caps =
        ",\"job:pty:create\",\"job:pty:resize\",\"job:pty:write\"";
    const char* pty_supported = "true";
#else
    const char* pty_caps = "";
    const char* pty_supported = "false";
#endif
    srv_dprintf(respfd,
            "},\"external_effect\":\"exec\",\"unknown_effect\":\"exec\","
            "\"limits\":{\"request_line_bytes\":%d,\"argv_items\":%d,"
            "\"env_items\":%d,\"decoded_stdin_buffer_bytes\":%d,"
            "\"inline_output_bytes\":%d},"
            "\"capabilities_required\":true,"
            "\"capabilities\":[\"effect:read\",\"effect:mutate\","
            "\"effect:exec\",\"job:create\",\"job:read\",\"job:signal\","
            "\"job:stdin:write\"%s],"
            "\"job_capabilities\":[\"effect:exec\",\"job:create\","
            "\"job:read\",\"job:signal\",\"job:stdin:write\",\"store:read\",\"store:manage\",\"data:create\",\"data:read\",\"data:manage\"%s],"
            "\"deadline_effects\":[\"effect:exec\"],"
            "\"text_line_authorized\":false,"
            "\"features\":{\"streaming\":false,\"background\":false,"
            "\"pty\":false,\"binary_stdin\":false,"
            "\"completed_state_in_response\":true},"
            "\"protocol_features\":{\"1\":{\"streaming\":false,"
            "\"binary_stdin\":false},\"2\":{\"streaming\":true,"
             "\"binary_stdin\":true,\"byte_exact_base64\":true,"
             "\"byte_argv\":true,\"byte_paths\":true,"
            "\"pipe_backpressure\":true,\"terminal_output_hashes\":true,"
            "\"content_addressed_output_refs\":true}},"
            "\"jobs\":{\"enabled\":true,\"max_jobs\":32,"
            "\"id_scope\":\"persistent-shared\","
            "\"start_idempotency\":\"exact-request-persistent\","
            "\"terminal_receipts\":true,\"start_is_terminal\":false,"
            "\"session_end\":{\"policy\":\"cancel-reap-archive\",\"result_op\":\"job.result\",\"reattach\":false},"
            "\"protected_env\":[\"HOME\",\"PWD\","
            "\"JASH_AGENT_CANCEL_FD\",\"JASH_AGENT_CGROUP_ROOT\"],"
            "\"features\":{\"exec\":true,\"background\":true,"
            "\"poll\":true,\"wait\":true,\"cancel_term_kill\":true,"
            "\"cwd\":true,\"env\":true,\"binary_stdin\":true,"
            "\"artifact_stdin\":true,\"terminal_stream_stdin\":true,"
            "\"stdin_pipe\":true,\"stdin_half_close\":true,"
            "\"pty\":%s,\"pty_resize\":%s,\"pty_write\":%s,"
            "\"paged_base64_read\":true,\"separate_spools\":true},"
            "\"resources\":{\"backend\":\"cgroup-v2\",\"configured\":%s,"
            "\"enforcement\":\"required-per-job-setup\",\"cpu_semantics\":\"bandwidth\","
            "\"swap_max_bytes\":0,\"oom_group\":true}},"
            "\"job_limits\":{\"jobs\":32,\"wait_ms\":30000,"
            "\"cancel_grace_ms\":5000,\"read_bytes\":65536,"
            "\"write_bytes\":16384,\"stdin_bytes\":16384,"
            "\"cwd_bytes\":4096,\"env_items\":16,\"env_bytes\":16384,"
            "\"env_key_bytes\":255,\"env_value_bytes\":2047,"
            "\"effective_env_items\":256,\"effective_env_bytes\":65536,"
            "\"output_bytes\":67108864,"
            "\"pipeline_stages\":16},"
            "\"cwd_encoding\":\"byte-preserving-u00\","
            "\"pwd_semantics\":\"logical-open-snapshot\","
            "\"cancel\":{\"transport\":\"fd-jsonl\",\"enabled\":%s,"
            "\"fd_env\":\"JASH_AGENT_CANCEL_FD\",\"kill_after_ms\":200,"
            "\"activation_ack\":true,\"kill_after_scope\":\"exec\","
            "\"native_mutators\":{\"mode\":\"cooperative\",\"required_first_argument\":\"--json\","
            "\"timeout_ms\":false,\"kill_after_ms\":null}},"
            "\"managed_shutdown\":%s,"
            "\"data_plane\":{\"version\":2,\"fd_lane\":%s,\"framing\":\"JDF1-seqpacket\","
            "\"stream_framing\":\"JDS1-seqpacket\",\"stream_delivery\":\"pull-credit\","
            "\"stream_chunk_bytes\":16384,\"stream_slots\":8,\"stream_window_bytes\":65536,"
            "\"stream_session_in_flight_bytes\":262144,\"stream_source\":\"bounded-job-spool\","
            "\"volatile_slots\":4,\"artifact_stdin\":\"read-only-fd\","
            "\"terminal_stream_stdin\":\"sealed-range-snapshot\","
            "\"artifact_stdin_fallback\":\"explicit-only\","
            "\"ids\":\"correlation-except-promotion\",\"promotion_ids\":\"persistent-exact-request\",\"durable_retention\":\"keep\"},"
            "\"artifact_store\":{\"root\":\"startup-home\",\"coordination\":\"session-leases\",\"root_retention\":\"explicit-release\","
            "\"quota_scope\":\"cas-and-job-archives-including-staging\",\"maintenance_ids\":\"persistent-exact-request\","
            "\"resolution\":\"verified-terminal-publication\",\"resolution_ids\":\"correlation-only\","
            "\"maintenance_inspection\":\"target-id\","
            "\"maintenance_history\":\"snapshot-cursor-v1\","
            "\"effect_protocol\":\"admission-v1\",\"preparation_resolution\":\"signed-closure\","
            "\"journal_quota\":\"logical-preparation-admission\",\"journal_recovery\":\"quota-exempt\","
            "\"journal_gc\":\"verified-result-staging\","
            "\"maintenance_namespace\":\"agent_maintenance\",\"inspection_ids\":\"correlation-only\"},"
            "\"receipts\":{\"required\":true,\"format\":\"dsse\","
            "\"schema\":\"jash.agent-runtime-receipt.v2\","
            "\"media_type\":"
            "\"application/vnd.jash.agent-runtime-receipt.v2+json\","
            "\"pubkey\":\"%s\",\"jobs\":{"
            "\"schema\":\"jash.agent-job-receipt.v1\","
            "\"media_type\":"
            "\"application/vnd.jash.agent-job-receipt.v1+json\","
            "\"phases\":[\"start\",\"action\",\"terminal\"]}}}\n",
            SRV_AGENT_REQ_MAX, SRV_ARGV_MAX, SRV_ENV_MAX, SRV_STDIN_MAX - 1,
            SRV_INLINE_MAX, pty_caps, pty_caps, pty_supported, pty_supported,
            pty_supported, job_table.cgroup_parent_fd >= 0 ? "true" : "false",
            s->agent_cancel_fd >= 0 ? "true" : "false", srv_shutdown_fd >= 0 ? "true" : "false", srv_data_fd >= 0 ? "true" : "false", signer.pubkey_hex);
#else
    srv_dprintf(respfd,
            "{\"jash_serve\":1,\"version\":\"%s\",\"pid\":%d,"
            "\"protocol\":\"jsonl\",\"fields\":[\"id\",\"argv\",\"line\","
            "\"stdin\",\"cwd\",\"env\",\"timeout_ms\"]}\n",
            JASH_VERSION, (int)getpid());
#endif

#ifdef JASH_AGENT_ONLY
    job_table.input_fd = fileno(in);
    job_table.cancel_fd = s->agent_cancel_fd;
#endif
    char* buf = (char*)malloc(SRV_REQ_MAX);
    if (!buf) {
#ifdef JASH_AGENT_ONLY
        if (job_table.cgroup_parent_fd >= 0) close(job_table.cgroup_parent_fd);
#endif
        fclose(in);
        close(respfd);
        return 1;
    }

    int seq = 0;
#ifdef JASH_AGENT_ONLY
    int response_closed = 0;
    srv_managed_input_t input = {.fd = fileno(in), .idle_ms = -1};
    struct timeval idle; socklen_t idle_size = sizeof(idle);
    if (srv_shutdown_fd >= 0 && !getsockopt(input.fd, SOL_SOCKET, SO_RCVTIMEO, &idle, &idle_size) && (idle.tv_sec || idle.tv_usec)) {
        long long ms = (long long)idle.tv_sec * 1000 + idle.tv_usec / 1000;
        input.idle_ms = ms > INT_MAX ? INT_MAX : ms > 0 ? (int)ms : 1;
    }
#endif
    for (;;) {
        if (srv_response_failed) break;
#ifdef JASH_AGENT_ONLY
        if (srv_shutdown_requested() || srv_store_poisoned) break;
#endif
        size_t l = 0;
        int ch = EOF;
        int oversized = 0;
        while ((ch =
#ifdef JASH_AGENT_ONLY
                srv_shutdown_fd >= 0 ? srv_managed_getc(&input) :
#endif
                fgetc(in)) != EOF && ch != '\n') {
            if (l + 1 < SRV_REQ_MAX) buf[l++] = (char)ch;
            else oversized = 1;
        }
#ifdef JASH_AGENT_ONLY
        if (srv_shutdown_requested()) break;
#endif
        if (ch == EOF && l == 0 && !oversized) break;
        if (oversized) {
            srv_dprintf(respfd, "{\"id\":null,\"error\":\"request too large\"}\n");
            continue;
        }
        if (memchr(buf, '\0', l)) {
            srv_dprintf(respfd, "{\"id\":null,\"error\":\"raw NUL in request\"}\n");
            continue;
        }
        buf[l] = '\0';
#ifdef JASH_AGENT_ONLY
        if (l > SRV_AGENT_REQ_MAX) {
            srv_dprintf(respfd, "{\"id\":null,\"error\":\"request too large\"}\n");
            continue;
        }
#endif
        const char* p = buf;
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (!*p) continue;
#ifdef JASH_AGENT_ONLY
        if (srv_probe_job_op(p))
            srv_job_dispatch(respfd, &job_table, p, &signer);
        else
#endif
            srv_handle(s, p, respfd, seq++, &signer);
#ifdef JASH_AGENT_ONLY
        if (srv_shutdown_requested()) break;
        if (srv_response_closed(respfd)) { response_closed = 1; break; }
#endif
        if (srv_response_failed || s->exit_requested) break;
    }
    free(buf);
#ifdef JASH_AGENT_ONLY
    const char* reason = srv_shutdown_state == 1 ? "listener_shutdown" : srv_shutdown_state == 2 ? "listener_lost" :
        s->exit_requested ? "session_exit" : response_closed || srv_response_failed ? "transport_closed" : input.error || ferror(in) ? "input_error" : "connection_eof";
    int cleanup_failed = srv_job_cleanup(&job_table, &signer, reason);
    if (job_table.cgroup_parent_fd >= 0) close(job_table.cgroup_parent_fd);
#endif
    fclose(in);
    close(respfd);
    memset(&signer, 0, sizeof signer);
#ifdef JASH_AGENT_ONLY
    if (cleanup_failed || srv_store_poisoned) return 125;
#endif
    /* Clean EOF = protocol completed: exit 0 regardless of the last
     * command's rc (each request already carried its own rc). Only an
     * explicit in-protocol `exit` propagates a code. */
    return srv_response_failed ? 125 : s->exit_requested ? s->last_status.code : 0;
}

int jash_serve_loop_controlled(jash_state_t* s, int shutdown_fd) {
    struct sigaction ignored = {0}, previous;
    ignored.sa_handler = SIG_IGN; sigemptyset(&ignored.sa_mask);
    if (sigaction(SIGPIPE, &ignored, &previous)) return 125;
#ifdef JASH_AGENT_ONLY
    int previous_fd = srv_shutdown_fd, previous_state = srv_shutdown_state;
    srv_shutdown_fd = shutdown_fd; srv_shutdown_state = 0;
    srv_store_poisoned = 0;
    if (srv_store_bind()) {
        srv_shutdown_fd = previous_fd; srv_shutdown_state = previous_state;
        sigaction(SIGPIPE, &previous, NULL); return 125;
    }
    int rc = srv_serve_loop(s);
    srv_data_cleanup();
    close(srv_store_lease); srv_store_lease = -1; srv_store_home[0] = 0;
    srv_shutdown_fd = previous_fd; srv_shutdown_state = previous_state;
#else
    (void)shutdown_fd;
    int rc = srv_serve_loop(s);
#endif
    srv_response_fd = -1;
    if (sigaction(SIGPIPE, &previous, NULL)) return 125;
    return rc;
}

int jash_serve_loop_data(jash_state_t* s, int shutdown_fd, int data_fd) {
#ifdef JASH_AGENT_ONLY
    int previous = srv_data_fd; srv_data_fd = data_fd;
    int result = jash_serve_loop_controlled(s, shutdown_fd); srv_data_fd = previous;
    return result;
#else
    (void)data_fd; return jash_serve_loop_controlled(s, shutdown_fd);
#endif
}

int jash_serve_loop(jash_state_t* s) { return jash_serve_loop_controlled(s, -1); }
