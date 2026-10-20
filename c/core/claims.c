// SPDX-License-Identifier: LicenseRef-SPL-1.0
// Checked shared file intent store. See claims.h for the advisory policy.
#define _GNU_SOURCE
#include "claims.h"
#include "mesh_store.h"

int jash_claims_live(const jash_claim_t* claim, long now) {
    return claim->ttl > 0 && (claim->claim_ts >= now ||
        (uint64_t)now - (uint64_t)claim->claim_ts <= (uint64_t)claim->ttl);
}

int jash_claims_read_at(int directory, jash_claim_t* claims, int maximum) {
    if (!claims || maximum < 0 || maximum > JASH_CLAIM_MAX) { errno = EINVAL; return -1; }
    char* bytes = NULL; size_t length = 0;
    int read_result = jash_mesh_read_at(directory, "claims.tsv", 2u * 1024u * 1024u, &bytes, &length);
    if (read_result <= 0) return read_result;
    if (memchr(bytes, 0, length) || (length && bytes[length - 1] != '\n')) {
        free(bytes); errno = EINVAL; return -1;
    }
    int count = 0, error = 0;
    char* cursor = bytes;
    while (*cursor) {
        if (count == maximum) { error = E2BIG; break; }
        char* end = strchr(cursor, '\n');
        if (!end || end == cursor) { error = EINVAL; break; }
        *end = 0;
        char* fields[6]; int n = jash_mesh_fields(cursor, fields, 6);
        int encoded = n == 6 && !strcmp(fields[5], "@jash-mesh-v2");
        if ((n != 4 && n != 5 && !encoded) || (n == 6 && !encoded)) { error = EINVAL; break; }
        jash_claim_t* claim = &claims[count];
        memset(claim, 0, sizeof *claim);
        uint64_t timestamp, ttl;
        if (jash_mesh_unescape(fields[0], claim->path, sizeof claim->path, encoded) ||
            jash_mesh_unescape(fields[1], claim->owner, sizeof claim->owner, encoded) ||
            jash_mesh_unescape(n >= 5 ? fields[4] : "-", claim->note, sizeof claim->note, encoded) ||
            jash_mesh_number(fields[2], LONG_MAX, &timestamp) ||
            jash_mesh_number(fields[3], LONG_MAX, &ttl) || !ttl ||
            claim->path[0] != '/' || !claim->owner[0]) { error = EINVAL; break; }
        claim->claim_ts = (long)timestamp; claim->ttl = (long)ttl;
        for (int i = 0; i < count; i++) if (!strcmp(claims[i].path, claim->path)) { error = EINVAL; break; }
        if (error) break;
        count++; cursor = end + 1;
    }
    free(bytes);
    if (error) { errno = error; return -1; }
    return count;
}

int jash_claims_write_at(int directory, const jash_claim_t* claims, int count, jash_mesh_io_t* outcome) {
    memset(outcome, 0, sizeof *outcome);
    if (count < 0 || count > JASH_CLAIM_MAX) { errno = EINVAL; outcome->error = errno; return -1; }
    if (!count) return jash_mesh_remove_at(directory, "claims.tsv", outcome);
    const size_t limit = 2u * 1024u * 1024u;
    char* buffer = malloc(limit);
    if (!buffer) { outcome->error = ENOMEM; return -1; }
    size_t used = 0; int error = 0;
    for (int i = 0; i < count && !error; i++) {
        const jash_claim_t* claim = &claims[i];
        char numeric[96];
        int n = snprintf(numeric, sizeof numeric, "\t%ld\t%ld\t", claim->claim_ts, claim->ttl);
        if (!memchr(claim->path, 0, sizeof claim->path) || !memchr(claim->owner, 0, sizeof claim->owner) ||
            !memchr(claim->note, 0, sizeof claim->note) || !claim->path[0] || !claim->owner[0] ||
            claim->claim_ts < 0 || claim->ttl <= 0 || n < 0 || (size_t)n >= sizeof numeric ||
            jash_mesh_escape(buffer, limit, &used, claim->path) || used + 1 >= limit) { error = EINVAL; break; }
        buffer[used++] = '\t';
        if (jash_mesh_escape(buffer, limit, &used, claim->owner) || (size_t)n >= limit - used) { error = E2BIG; break; }
        memcpy(buffer + used, numeric, (size_t)n); used += (size_t)n;
        if (jash_mesh_escape(buffer, limit, &used, claim->note) || sizeof("\t@jash-mesh-v2\n") >= limit - used) { error = E2BIG; break; }
        memcpy(buffer + used, "\t@jash-mesh-v2\n", sizeof("\t@jash-mesh-v2\n") - 1);
        used += sizeof("\t@jash-mesh-v2\n") - 1;
    }
    int result = error ? -1 : jash_mesh_write_at(directory, "claims.tsv", buffer, used, outcome);
    free(buffer);
    if (error) { outcome->error = error; errno = error; }
    return result;
}

int jash_claims_norm(const char* path, char* output, size_t capacity) {
    if (!path || !*path || !output || !capacity) { errno = EINVAL; return 0; }
    char joined[PATH_MAX], resolved[PATH_MAX], current[PATH_MAX];
    if (strlen(path) >= sizeof joined) { errno = ENAMETOOLONG; return 0; }
    if (path[0] == '/') memcpy(joined, path, strlen(path) + 1);
    else {
        if (!getcwd(joined, sizeof joined)) return 0;
        size_t n = strlen(joined), m = strlen(path);
        if (n + m + 2 > sizeof joined) { errno = ENAMETOOLONG; return 0; }
        joined[n++] = '/'; memcpy(joined + n, path, m + 1);
    }
    current[0] = '/'; current[1] = 0;
    char* cursor = joined;
    while (*cursor) {
        while (*cursor == '/') cursor++;
        if (!*cursor) break;
        char* end = strchr(cursor, '/');
        if (end) *end = 0;
        if (!strcmp(cursor, "..")) {
            char* slash = strrchr(current, '/');
            if (slash != current) *slash = 0; else current[1] = 0;
        } else if (strcmp(cursor, ".")) {
            size_t n = strlen(current), m = strlen(cursor);
            if (n + m + 2 > sizeof current) { errno = ENAMETOOLONG; return 0; }
            if (n > 1) current[n++] = '/';
            memcpy(current + n, cursor, m + 1);
            if (realpath(current, resolved)) memcpy(current, resolved, strlen(resolved) + 1);
            else if (errno != ENOENT && errno != ENOTDIR) return 0;
        }
        if (!end) break;
        cursor = end + 1;
    }
    if (strlen(current) >= capacity) { errno = ENAMETOOLONG; return 0; }
    memcpy(output, current, strlen(current) + 1); return 1;
}

static int claims_finish(jash_mesh_store_t* store, int result) {
    int saved = result < 0 ? errno : 0;
    if (jash_mesh_store_close(store) && !saved) saved = errno;
    if (saved) { errno = saved; return -1; }
    return result;
}

int jash_claims_put(const char* path, const char* owner, long ttl, const char* note) {
    if (!path || !*path || !owner || !*owner || strlen(path) >= JASH_CLAIM_PATH_MAX ||
        strlen(owner) >= JASH_CLAIM_OWNER_MAX || (note && strlen(note) >= JASH_CLAIM_NOTE_MAX)) { errno = EINVAL; return -1; }
    if (ttl <= 0) ttl = JASH_CLAIM_DEFAULT_TTL;
    jash_mesh_store_t store;
    if (jash_mesh_store_open(&store, 1)) return claims_finish(&store, -1);
    jash_claim_t* claims = calloc(JASH_CLAIM_MAX, sizeof *claims);
    if (!claims) return claims_finish(&store, -1);
    int count = jash_claims_read_at(store.root, claims, JASH_CLAIM_MAX), index = -1;
    long now = (long)time(NULL);
    int result = count < 0 || now < 0 ? -1 : 0;
    for (int i = 0; i < count && !result; i++) if (!strcmp(claims[i].path, path)) {
        if (jash_claims_live(&claims[i], now) && strcmp(claims[i].owner, owner)) result = 1;
        index = i; break;
    }
    if (!result) {
        if (index < 0) {
            if (count == JASH_CLAIM_MAX) { errno = E2BIG; result = -1; }
            else index = count++;
        }
        if (!result) {
            jash_claim_t* claim = &claims[index];
            memcpy(claim->path, path, strlen(path) + 1); memcpy(claim->owner, owner, strlen(owner) + 1);
            const char* selected = note ? note : "-";
            memcpy(claim->note, selected, strlen(selected) + 1); claim->claim_ts = now; claim->ttl = ttl;
            jash_mesh_io_t outcome;
            result = jash_claims_write_at(store.root, claims, count, &outcome);
        }
    }
    free(claims); return claims_finish(&store, result);
}

static int claims_owner_change(const char* path, const char* owner, int refresh) {
    if (!owner || !*owner) { errno = EINVAL; return -1; }
    jash_mesh_store_t store;
    if (jash_mesh_store_open(&store, 1)) return claims_finish(&store, -1);
    jash_claim_t* claims = calloc(JASH_CLAIM_MAX, sizeof *claims);
    if (!claims) return claims_finish(&store, -1);
    int count = jash_claims_read_at(store.root, claims, JASH_CLAIM_MAX), changed = 0, kept = 0;
    long now = (long)time(NULL);
    int result = count < 0 || now < 0 ? -1 : 0;
    for (int i = 0; i < count && !result; i++) {
        int selected = !strcmp(claims[i].owner, owner) && (!path || !strcmp(claims[i].path, path));
        if (selected) {
            changed++;
            if (!refresh) continue;
            claims[i].claim_ts = now;
        }
        claims[kept++] = claims[i];
    }
    if (!result && changed) {
        jash_mesh_io_t outcome;
        result = jash_claims_write_at(store.root, claims, kept, &outcome);
    }
    free(claims); return claims_finish(&store, result ? result : changed);
}

int jash_claims_remove(const char* path, const char* owner) { return claims_owner_change(path, owner, 0); }
int jash_claims_refresh_owner(const char* owner) { return claims_owner_change(NULL, owner, 1); }
int jash_claims_drop_owner(const char* owner) { return claims_owner_change(NULL, owner, 0); }

int jash_claims_load_all(jash_claim_t* output, int maximum) {
    if (!output || maximum < 0) { errno = EINVAL; return -1; }
    jash_mesh_store_t store;
    int opened = jash_mesh_store_open(&store, 0);
    if (opened) return claims_finish(&store, opened < 0 ? -1 : 0);
    jash_claim_t* all = calloc(JASH_CLAIM_MAX, sizeof *all);
    if (!all) return claims_finish(&store, -1);
    int count = jash_claims_read_at(store.root, all, JASH_CLAIM_MAX), live = 0;
    long now = (long)time(NULL);
    int result = count < 0 || now < 0 ? -1 : 0;
    for (int i = 0; i < count && !result; i++) if (jash_claims_live(&all[i], now)) {
        if (live == maximum) { errno = E2BIG; result = -1; break; }
        output[live++] = all[i];
    }
    free(all); return claims_finish(&store, result ? result : live);
}

int jash_claims_conflict(const char* path, const char* self, char* owner, size_t owner_capacity, long* ttl) {
    if (!path || !*path) { errno = EINVAL; return -1; }
    jash_claim_t* all = calloc(JASH_CLAIM_MAX, sizeof *all);
    if (!all) return -1;
    int count = jash_claims_load_all(all, JASH_CLAIM_MAX), result = count < 0 ? -1 : 0;
    for (int i = 0; i < count; i++) if (!strcmp(all[i].path, path) && strcmp(all[i].owner, self ? self : "")) {
        if (owner && owner_capacity) {
            if (strlen(all[i].owner) >= owner_capacity) { errno = E2BIG; result = -1; break; }
            memcpy(owner, all[i].owner, strlen(all[i].owner) + 1);
        }
        if (ttl) *ttl = all[i].ttl;
        result = 1; break;
    }
    free(all); return result;
}

#define CLAIM_CONF_MAX 64
static char g_conf[CLAIM_CONF_MAX][JASH_CLAIM_PATH_MAX];
static int g_conf_n, g_conf_truncated;
void jash_claims_note_conflict(const char* path) {
    if (!path || !*path) return;
    for (int i = 0; i < g_conf_n; i++) if (!strcmp(g_conf[i], path)) return;
    if (g_conf_n == CLAIM_CONF_MAX || strlen(path) >= JASH_CLAIM_PATH_MAX) { g_conf_truncated = 1; return; }
    memcpy(g_conf[g_conf_n++], path, strlen(path) + 1);
}
int jash_claims_conflicts(const char** output, int maximum) {
    int count = g_conf_n < maximum ? g_conf_n : maximum;
    for (int i = 0; i < count; i++) output[i] = g_conf[i];
    return count;
}
int jash_claims_conflicts_truncated(void) { return g_conf_truncated; }
