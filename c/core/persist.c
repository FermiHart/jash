// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// Module: core/persist — persistent shell state implementation
//
// State file format (~/.jash_state):
//   # JASH STATE FILE
//   [CWD]
//   /path/to/dir
//   [ENV]
//   KEY=VALUE
//   [ALIAS]
//   name=expansion

#include "persist.h"
#include "env.h"
#include "alias.h"
#include <sys/syscalls.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>
#include <limits.h>

static int persist_fail(jash_persist_result_t* result, const char* code, int number) {
    result->error = code;
    result->error_number = number ? number : EIO;
    errno = result->error_number;
    return -1;
}

int jash_persist_path(char* buffer, size_t capacity) {
    const char* home = getenv("HOME");
    if (!home || !home[0]) home = jash_tmpdir();
    int length = snprintf(buffer, capacity, "%s/.jash_state", home);
    if (length < 0 || (size_t)length >= capacity) { errno = ENAMETOOLONG; return -1; }
    return 0;
}

typedef struct { long fd; size_t bytes; } persist_writer_t;
static int persist_write_all(persist_writer_t* writer, const char* data, size_t len) {
    if (len > JASH_PERSIST_MAX_BYTES - writer->bytes) { errno = EFBIG; return -1; }
    while (len) {
        long n = sys_write(writer->fd, data, (unsigned long)len);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { if (!n) errno = EIO; return -1; }
        data += n; len -= (size_t)n; writer->bytes += (size_t)n;
    }
    return 0;
}

static int persist_write_entry(persist_writer_t* writer, const char* key, const char* value) {
    static const char hex[] = "0123456789abcdef";
    size_t key_len = strlen(key), value_len = strlen(value);
    if (value_len > (SIZE_MAX - key_len - 2) / 2) { errno = EOVERFLOW; return -1; }
    size_t len = key_len + value_len * 2 + 2;
    if (len > JASH_PERSIST_MAX_BYTES - writer->bytes) { errno = EFBIG; return -1; }
    char* line = malloc(len);
    if (!line) return -1;
    memcpy(line, key, key_len); line[key_len] = '=';
    for (size_t i = 0; i < value_len; i++) {
        unsigned char c = (unsigned char)value[i];
        line[key_len + 1 + i * 2] = hex[c >> 4];
        line[key_len + 2 + i * 2] = hex[c & 15];
    }
    line[len - 1] = '\n';
    int rc = persist_write_all(writer, line, len);
    int saved = errno; free(line); errno = saved;
    return rc;
}

/* Publication and durability are distinct outcomes. */
static int persist_sync_parent(const char* path) {
    char directory[PATH_MAX];
    size_t length = strlen(path);
    if (length >= sizeof directory) { errno = ENAMETOOLONG; return -1; }
    memcpy(directory, path, length + 1u);
    char* slash = strrchr(directory, '/');
    if (slash) *slash = 0; else memcpy(directory, ".", 2);
    int fd = open(directory[0] ? directory : "/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return -1;
    int rc = fsync(fd), saved = errno;
    if (close(fd) && !rc) { rc = -1; saved = errno; }
    errno = saved; return rc;
}

typedef struct {
    char** keys;
    char** values;
    size_t count;
    size_t cap;
} persist_pairs_t;

static void persist_pairs_free(persist_pairs_t* pairs) {
    for (size_t i = 0; i < pairs->count; i++) {
        free(pairs->keys[i]);
        free(pairs->values[i]);
    }
    free(pairs->keys); free(pairs->values);
    memset(pairs, 0, sizeof *pairs);
}

static int persist_pairs_push(persist_pairs_t* pairs, const char* key,
                              const char* value) {
    if (pairs->count == pairs->cap) {
        size_t next = pairs->cap ? pairs->cap * 2 : 32;
        if (next < pairs->cap || next > SIZE_MAX / sizeof(char*)) return -1;
        char** keys = (char**)realloc(pairs->keys, next * sizeof(char*));
        if (!keys) return -1;
        pairs->keys = keys;
        char** values = (char**)realloc(pairs->values, next * sizeof(char*));
        if (!values) return -1;
        pairs->values = values;
        pairs->cap = next;
    }
    char* key_copy = strdup(key);
    char* value_copy = strdup(value);
    if (!key_copy || !value_copy) {
        free(key_copy); free(value_copy);
        return -1;
    }
    pairs->keys[pairs->count] = key_copy;
    pairs->values[pairs->count] = value_copy;
    pairs->count++;
    return 0;
}

static int persist_hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static char* persist_hex_decode(const char* encoded) {
    size_t len = strlen(encoded);
    if (len & 1) return NULL;
    char* out = (char*)malloc(len / 2 + 1);
    if (!out) return NULL;
    for (size_t i = 0; i < len; i += 2) {
        int hi = persist_hex_nibble(encoded[i]);
        int lo = persist_hex_nibble(encoded[i + 1]);
        if (hi < 0 || lo < 0) { free(out); return NULL; }
        out[i / 2] = (char)((hi << 4) | lo);
        if (out[i / 2] == 0) { free(out); return NULL; }
    }
    out[len / 2] = 0;
    return out;
}

/* Wrappers remain quiet for automatic persistence; explicit callers receive
 * the actual read/write/publication/durability outcome. */
static int persist_write_to(jash_state_t* s, const char* path, jash_persist_result_t* result) {
    /* V2 hex-encodes values, while keys are line delimiters. Reject keys
     * which that format cannot round-trip instead of publishing data loss. */
    jash_env_sync_frame(s);
    for (uint32_t i = 0; i < s->env_count; i++) {
        const char* key = s->env[i].key;
        if (strchr(key, '[')) continue;
        if (!key[0] || key[0] == '#' || strpbrk(key, "=\r\n"))
            return persist_fail(result, "unrepresentable_key", EINVAL);
    }
    for (uint32_t i = 0; i < s->alias_count; i++) {
        const char* key = s->aliases[i].key;
        if (!key[0] || key[0] == '#' || key[0] == '[' || strpbrk(key, "=\r\n"))
            return persist_fail(result, "unrepresentable_key", EINVAL);
    }
    char temporary[PATH_MAX];
    int length = snprintf(temporary, sizeof temporary, "%s.tmp.XXXXXX", path);
    if (length < 0 || (size_t)length >= sizeof temporary) return persist_fail(result, "path_too_long", ENAMETOOLONG);
    int fd = mkstemp(temporary);
    if (fd < 0) return persist_fail(result, "open_failed", errno);
    persist_writer_t writer = {fd, 0};
    const char* error = "write_failed";
    if (fchmod(fd, 0600)) { error = "chmod_failed"; goto failed; }
    static const char header[] = "# JASH STATE FILE v2\n[CWD2]\n";
    if (persist_write_all(&writer, header, sizeof header - 1u)) goto failed;
    if (s->cwd[0] == '/' && s->cwd[1] && persist_write_entry(&writer, "cwd", s->cwd)) goto failed;
    if (persist_write_all(&writer, "[ENV2]\n", 7)) goto failed;
    for (uint32_t i = 0; i < s->env_count; i++) {
        if (strchr(s->env[i].key, '[')) continue;
        if (persist_write_entry(&writer, s->env[i].key, JASH_ENV_VAL(&s->env[i]))) goto failed;
        result->env_entries++;
    }
    if (persist_write_all(&writer, "[ALIAS2]\n", 9)) goto failed;
    for (uint32_t i = 0; i < s->alias_count; i++) {
        if (persist_write_entry(&writer, s->aliases[i].key, s->aliases[i].val)) goto failed;
        result->alias_entries++;
    }
    if (persist_write_all(&writer, "[END]\n", 6)) goto failed;
    if (fsync(fd)) { error = "fsync_failed"; goto failed; }
    if (close(fd)) { int saved = errno; unlink(temporary); return persist_fail(result, "close_failed", saved); }
    if (rename(temporary, path)) { int saved = errno; unlink(temporary); return persist_fail(result, "rename_failed", saved); }
    result->applied = 1;
    if (persist_sync_parent(path)) return persist_fail(result, "directory_sync_failed", errno);
    result->durable = 1;
    return 0;
failed: {
    int saved = errno;
    close(fd); unlink(temporary);
    return persist_fail(result, saved == EFBIG ? "state_limit_exceeded" : error, saved);
}
}

/* The restore operation merges saved env/aliases. A failed CWD change after
 * that merge is reported as applied with an error, never as full success. */
static int persist_read_from(jash_state_t* s, const char* path, int force_cwd, jash_persist_result_t* result) {
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return persist_fail(result, "open_failed", errno);
    struct stat st;
    if (fstat(fd, &st)) {
        int saved = errno; close(fd); return persist_fail(result, "invalid_state_file", saved);
    }
    if (!S_ISREG(st.st_mode)) { close(fd); return persist_fail(result, "invalid_state_file", EINVAL); }
    size_t cap = 8192, used = 0;
    char* filebuf = malloc(cap);
    if (!filebuf) { close(fd); return persist_fail(result, "allocation_failed", ENOMEM); }
    const char* read_error = NULL; int read_errno = 0;
    for (;;) {
        if (used + 1u == cap) {
            if (cap == JASH_PERSIST_MAX_BYTES + 1u) {
                char extra;
                ssize_t n;
                do { n = read(fd, &extra, 1u); } while (n < 0 && errno == EINTR);
                if (n > 0) { read_error = "state_limit_exceeded"; read_errno = EFBIG; }
                else if (n < 0) { read_error = "read_failed"; read_errno = errno; }
                break;
            }
            size_t next = cap * 2u;
            if (next > JASH_PERSIST_MAX_BYTES + 1u) next = JASH_PERSIST_MAX_BYTES + 1u;
            char* grown = realloc(filebuf, next);
            if (!grown) { read_error = "allocation_failed"; read_errno = ENOMEM; break; }
            filebuf = grown; cap = next;
        }
        ssize_t n = read(fd, filebuf + used, cap - used - 1u);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { read_error = "read_failed"; read_errno = errno; break; }
        if (!n) break;
        used += (size_t)n;
    }
    if (close(fd) && !read_error) { read_error = "close_failed"; read_errno = errno; }
    if (read_error) { free(filebuf); return persist_fail(result, read_error, read_errno); }
    if (!used || memchr(filebuf, 0, used)) { free(filebuf); return persist_fail(result, "invalid_state_file", EINVAL); }
    filebuf[used] = 0;
    persist_pairs_t env = {0}, aliases = {0};
    char* staged_cwd = NULL;
    jash_state_t* alias_candidate = NULL;
    const char* error = "invalid_state_file"; int number = EINVAL;
    const char* section = NULL;
    int v2 = 0, complete = 0, saw_section = 0;
    char* save = NULL;
    for (char* line = strtok_r(filebuf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (line[0] == '#') continue;
        if (line[0] == '[') {
            if (complete) goto failed;
            char* end = strchr(line + 1, ']');
            if (!end || end[1]) goto failed;
            *end = 0; section = line + 1; saw_section = 1;
            if (!strcmp(section, "CWD2") || !strcmp(section, "ENV2") || !strcmp(section, "ALIAS2")) v2 = 1;
            if (!strcmp(section, "END")) complete = 1;
            else if (strcmp(section, "CWD") && strcmp(section, "CWD2") && strcmp(section, "ENV") &&
                     strcmp(section, "ENV2") && strcmp(section, "ALIAS") && strcmp(section, "ALIAS2")) goto failed;
            continue;
        }
        if (!section || complete) goto failed;
        if (!strcmp(section, "CWD")) {
            free(staged_cwd); staged_cwd = strdup(line);
            if (!staged_cwd) { error = "allocation_failed"; number = ENOMEM; goto failed; }
            continue;
        }
        char* equal = strchr(line, '=');
        if (!equal) goto failed;
        *equal = 0;
        int encoded = !strcmp(section, "CWD2") || !strcmp(section, "ENV2") || !strcmp(section, "ALIAS2");
        char* value = encoded ? persist_hex_decode(equal + 1) : strdup(equal + 1);
        if (!value) goto failed;
        if (!strcmp(section, "CWD2")) {
            if (strcmp(line, "cwd")) { free(value); goto failed; }
            free(staged_cwd); staged_cwd = value; continue;
        }
        int alias = !strcmp(section, "ALIAS") || !strcmp(section, "ALIAS2");
        size_t key_length = strlen(line);
        if (!key_length || key_length >= (alias ? JASH_ALIAS_KEY_LEN : JASH_ENV_KEY_LEN) ||
            (alias && strlen(value) >= JASH_ALIAS_VAL_LEN)) { free(value); goto failed; }
        persist_pairs_t* pairs = alias ? &aliases : &env;
        if (pairs->count == (alias ? JASH_ALIAS_MAX : JASH_MAX_ENV)) { free(value); goto failed; }
        int rc = persist_pairs_push(pairs, line, value); free(value);
        if (rc) { error = "allocation_failed"; number = ENOMEM; goto failed; }
    }
    if (!saw_section || (v2 && !complete) || (staged_cwd && (staged_cwd[0] != '/' || strlen(staged_cwd) >= sizeof s->cwd))) goto failed;
    alias_candidate = calloc(1, sizeof *alias_candidate);
    if (!alias_candidate) { error = "allocation_failed"; number = ENOMEM; goto failed; }
    memcpy(alias_candidate->aliases, s->aliases, sizeof s->aliases);
    alias_candidate->alias_count = s->alias_count;
    for (size_t i = 0; i < aliases.count; i++) if (jash_alias_set(alias_candidate, aliases.keys[i], aliases.values[i])) {
        error = "alias_table_full"; number = ENOSPC; goto failed;
    }
    if (jash_env_apply_batch(s, (const char* const*)env.keys, (const char* const*)env.values, (int)env.count)) {
        error = "environment_apply_failed"; number = ENOMEM; goto failed;
    }
    memcpy(s->aliases, alias_candidate->aliases, sizeof s->aliases); s->alias_count = alias_candidate->alias_count;
    result->applied = 1; result->env_entries = env.count; result->alias_entries = aliases.count;
    if (staged_cwd && (force_cwd || s->interactive)) {
        if (sys_chdir(staged_cwd)) {
            error = "cwd_restore_failed"; number = errno;
            /* The environment merge may contain the failed destination as
             * PWD. Preserve the actual directory while reporting the partial
             * restore as a failure; a saved path is not an observation. */
            char actual[sizeof s->cwd];
            if (getcwd(actual, sizeof actual)) {
                memcpy(s->cwd, actual, strlen(actual) + 1u);
                if (jash_env_set(s, "PWD", s->cwd)) {
                    error = "pwd_update_failed"; number = ENOMEM;
                }
            }
            goto failed;
        }
        memcpy(s->cwd, staged_cwd, strlen(staged_cwd) + 1u); result->cwd_applied = 1;
        if (jash_env_set(s, "PWD", s->cwd)) { error = "pwd_update_failed"; number = ENOMEM; goto failed; }
    }
    persist_pairs_free(&env); persist_pairs_free(&aliases); free(alias_candidate); free(staged_cwd); free(filebuf);
    return 0;
failed:
    persist_pairs_free(&env); persist_pairs_free(&aliases); free(alias_candidate); free(staged_cwd); free(filebuf);
    return persist_fail(result, error, number);
}

int jash_persist_save_checked(jash_state_t* s, jash_persist_result_t* result) {
    memset(result, 0, sizeof *result);
    if (!s) return persist_fail(result, "invalid_state", EINVAL);
    char path[PATH_MAX];
    if (jash_persist_path(path, sizeof path)) return persist_fail(result, "path_too_long", errno);
    return persist_write_to(s, path, result);
}

int jash_persist_load_checked(jash_state_t* s, jash_persist_result_t* result) {
    memset(result, 0, sizeof *result);
    if (!s) return persist_fail(result, "invalid_state", EINVAL);
    char path[PATH_MAX];
    if (jash_persist_path(path, sizeof path)) return persist_fail(result, "path_too_long", errno);
    return persist_read_from(s, path, 0, result);
}

int jash_persist_clear_checked(jash_state_t* s, jash_persist_result_t* result) {
    (void)s;
    memset(result, 0, sizeof *result);
    char path[PATH_MAX];
    if (jash_persist_path(path, sizeof path)) return persist_fail(result, "path_too_long", errno);
    if (unlink(path)) {
        if (errno == ENOENT) return 0;
        return persist_fail(result, "unlink_failed", errno);
    }
    result->applied = 1;
    if (persist_sync_parent(path)) return persist_fail(result, "directory_sync_failed", errno);
    result->durable = 1;
    return 0;
}

void jash_persist_save(jash_state_t* s) { jash_persist_result_t result; (void)jash_persist_save_checked(s, &result); }
void jash_persist_load(jash_state_t* s) { jash_persist_result_t result; (void)jash_persist_load_checked(s, &result); }
void jash_persist_clear(jash_state_t* s) { jash_persist_result_t result; (void)jash_persist_clear_checked(s, &result); }

/* ── F1: session-scoped persistence (JASH_SESSION) ───────────────────────── */

int jash_session_path(char* buf, unsigned long cap) {
    const char* sid = getenv("JASH_SESSION");
    if (!sid || !sid[0]) return 0;
    /* Sanitize the id: keep [A-Za-z0-9_-], bounded length. */
    char safe[128];
    unsigned o = 0;
    for (unsigned i = 0; sid[i] && o + 1 < sizeof(safe); i++) {
        char c = sid[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_')
            safe[o++] = c;
    }
    safe[o] = '\0';
    if (!o) return 0;
    const char* home = getenv("HOME");
    if (!home) home = jash_tmpdir();
    int length = snprintf(buf, cap, "%s/.jash/sessions/%s.state", home, safe);
    return length >= 0 && (unsigned long)length < cap;
}

void jash_session_load(jash_state_t* s) {
    if (!s) return;
    char path[512];
    if (!jash_session_path(path, sizeof(path))) return;
    jash_persist_result_t result = {0};
    (void)persist_read_from(s, path, /*force_cwd=*/1, &result);
}

/* Dirty checkpoint for the automatic per-command save: serializing the whole
 * env plus 2 fsyncs only pays off when something actually changed. Env
 * mutations bump s->env_generation (ADR-0036 W5); aliases have no generation
 * counter, so they are covered by a cheap FNV-1a pass over keys+vals. A
 * missing state file always forces a write — persist_write_to fails silently
 * on full/read-only filesystems, so "clean" must not imply "on disk". */
static uint64_t persist_alias_fingerprint(const jash_state_t* s) {
    uint64_t h = 1469598103934665603ULL;
    for (uint32_t i = 0; i < s->alias_count; i++) {
        for (const char* p = s->aliases[i].key; *p; p++)
            { h ^= (unsigned char)*p; h *= 1099511628211ULL; }
        h ^= 0xff; h *= 1099511628211ULL;
        for (const char* p = s->aliases[i].val; *p; p++)
            { h ^= (unsigned char)*p; h *= 1099511628211ULL; }
        h ^= 0xfe; h *= 1099511628211ULL;
    }
    return h;
}

void jash_session_save(jash_state_t* s) {
    if (!s) return;
    char path[512];
    if (!jash_session_path(path, sizeof(path))) return;
    static int ckpt_valid = 0;
    static char ckpt_path[512];
    static char ckpt_cwd[JASH_CWD_MAX];
    static uint64_t ckpt_env_gen, ckpt_alias_fp;
    uint64_t alias_fp = persist_alias_fingerprint(s);
    if (ckpt_valid && strcmp(ckpt_path, path) == 0 &&
        strcmp(ckpt_cwd, s->cwd) == 0 &&
        ckpt_env_gen == s->env_generation &&
        ckpt_alias_fp == alias_fp &&
        access(path, F_OK) == 0)
        return;  /* nothing persistable changed since the last save */
    /* Ensure ~/.jash/sessions exists. Best-effort — Beyond tmpfs may be
     * full; mkdir failure is silently ignored (persist_write_to will also
     * fail gracefully). F E R M I ∞ H A R T */
    const char* home = getenv("HOME");
    if (home) {
        char dir[400];
        snprintf(dir, sizeof(dir), "%s/.jash", home);        mkdir(dir, 0700);
        snprintf(dir, sizeof(dir), "%s/.jash/sessions", home); mkdir(dir, 0700);
    }
    jash_persist_result_t result = {0};
    if (persist_write_to(s, path, &result)) return;
    snprintf(ckpt_path, sizeof ckpt_path, "%s", path);
    snprintf(ckpt_cwd, sizeof ckpt_cwd, "%s", s->cwd);
    ckpt_env_gen = s->env_generation;
    ckpt_alias_fp = alias_fp;
    ckpt_valid = 1;
}
