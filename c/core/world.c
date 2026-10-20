// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — core/world — world-delta scanner (ADR-0042 F2). F E R M I ∞ H A R T
#define _GNU_SOURCE
#include "world.h"
#include "chain_read.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>

// Ignore list — directories to skip during scan.
static const char* const WORLD_IGNORE[] = {
    ".git", "node_modules", ".jash", ".cache", ".svn", ".hg",
    "build", "build-speed", "__pycache__", ".DS_Store", NULL
};

static int should_ignore(const char* name) {
    for (int i = 0; WORLD_IGNORE[i]; i++)
        if (strcmp(name, WORLD_IGNORE[i]) == 0) return 1;
    return 0;
}

// FNV-1a hash for path → bucket index.
static unsigned fnv1a(const char* s) {
    unsigned h = 2166136261u;
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 16777619u; }
    return h;
}

#define WORLD_HT_SIZE 4096

typedef struct ht_node {
    char path[JASH_WORLD_PATH_MAX];
    jash_world_entry_t entry;
    struct ht_node* next;
} ht_node_t;

typedef struct {
    ht_node_t* buckets[WORLD_HT_SIZE];
} world_ht_t;

static void ht_init(world_ht_t* ht) { memset(ht, 0, sizeof(*ht)); }

static int ht_insert(world_ht_t* ht, const jash_world_entry_t* e) {
    unsigned b = fnv1a(e->path) % WORLD_HT_SIZE;
    ht_node_t* n = (ht_node_t*)malloc(sizeof(ht_node_t));
    if (!n) return -1;
    memcpy(&n->entry, e, sizeof(*e));
    snprintf(n->path, sizeof(n->path), "%s", e->path);
    n->next = ht->buckets[b];
    ht->buckets[b] = n;
    return 0;
}

static void ht_free(world_ht_t* ht) {
    for (int i = 0; i < WORLD_HT_SIZE; i++) {
        ht_node_t* n = ht->buckets[i];
        while (n) { ht_node_t* next = n->next; free(n); n = next; }
    }
}

// Recursive scan helper.
static void scan_dir(world_ht_t* ht, const char* base, const char* rel,
                     int depth, int max_depth, int* count, int* failed) {
    if (depth > max_depth || *failed) return;
    char full[1024];
    int length = rel[0] ? snprintf(full, sizeof(full), "%s/%s", base, rel) :
                         snprintf(full, sizeof(full), "%s", base);
    if (length < 0 || (size_t)length >= sizeof(full)) { *failed = 1; return; }

    DIR* d = opendir(full);
    if (!d) { *failed = 1; return; }
    struct dirent* ent;
    while (!*failed && (ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') {
            if (ent->d_name[1] == '\0') continue;
            if (ent->d_name[1] == '.' && ent->d_name[2] == '\0') continue;
        }
        if (should_ignore(ent->d_name)) continue;
        char child_rel[1024];
        length = rel[0] ? snprintf(child_rel, sizeof(child_rel), "%s/%s", rel, ent->d_name) :
                          snprintf(child_rel, sizeof(child_rel), "%s", ent->d_name);
        if (length < 0 || (size_t)length >= sizeof(child_rel)) { *failed = 1; break; }

        struct stat st;
        char child_full[1024];
        length = snprintf(child_full, sizeof(child_full), "%s/%s", full, ent->d_name);
        if (length < 0 || (size_t)length >= sizeof(child_full)) { *failed = 1; break; }
        if (stat(child_full, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            scan_dir(ht, base, child_rel, depth + 1, max_depth, count, failed);
        } else if (S_ISREG(st.st_mode)) {
            if (*count >= JASH_WORLD_MAX_ENTRIES || strlen(child_rel) >= JASH_WORLD_PATH_MAX) {
                *failed = 1; break;
            }
            jash_world_entry_t e = {0};
            snprintf(e.path, sizeof(e.path), "%s", child_rel);
            e.mtime = (long)st.st_mtime;
            e.size = (long)st.st_size;
            e.mode = (int)st.st_mode;
            if (ht_insert(ht, &e) != 0) { *failed = 1; break; }
            (*count)++;
        }
    }
    closedir(d);
}

// qsort comparator: order entries by path for deterministic output.
static int entry_path_cmp(const void* a, const void* b) {
    return strcmp(((const jash_world_entry_t*)a)->path,
                  ((const jash_world_entry_t*)b)->path);
}

jash_world_scan_t* jash_world_scan(const char* root, int max_depth) {
    if (!root || !*root) return NULL;
    world_ht_t* ht = (world_ht_t*)malloc(sizeof(world_ht_t));
    if (!ht) return NULL;
    ht_init(ht);
    int count = 0, failed = 0;
    scan_dir(ht, root, "", 0, max_depth, &count, &failed);
    if (failed) { ht_free(ht); free(ht); return NULL; }

    // Flatten hash table into a sorted array.
    jash_world_scan_t* scan = (jash_world_scan_t*)malloc(sizeof(jash_world_scan_t));
    if (!scan) { ht_free(ht); free(ht); return NULL; }
    scan->entries = (jash_world_entry_t*)malloc((size_t)(count ? count : 1) * sizeof(jash_world_entry_t));
    scan->count = count;
    scan->capacity = count;
    if (!scan->entries) { free(scan); ht_free(ht); free(ht); return NULL; }
    int idx = 0;
    for (int i = 0; i < WORLD_HT_SIZE; i++) {
        for (ht_node_t* n = ht->buckets[i]; n; n = n->next)
            scan->entries[idx++] = n->entry;
    }
    // Sort by path for deterministic output.
    qsort(scan->entries, (size_t)count, sizeof(jash_world_entry_t),
          entry_path_cmp);
    ht_free(ht);
    free(ht);
    return scan;
}

// Append a quoted path to a comma-separated JSON array body, bounded.
// Caps at 20 entries and never overruns `cap` (ADR-0042: arrays are bounded).
static void wd_append(char* dst, size_t cap, int* n, int* total, const char* path) {
    (*total)++;
    if (*n >= 20) return;
    char q[6 * JASH_WORLD_PATH_MAX + 4] = "";
    jash_json_buffer_t buffer = {q, sizeof(q), 0};
    jash_json_writer_t writer;
    jash_jsonw_init(&writer, jash_jsonw_buffer_sink, &buffer);
    jash_jsonw_string_or_bytes(&writer, path);
    if (jash_jsonw_finish(&writer) != 0) return;
    size_t cur = strlen(dst);
    size_t need = cur + (cur ? 1u : 0u) + buffer.used;
    if (need >= cap) return; /* total vs n exposes the bounded sample */
    if (cur) strlcat(dst, ",", cap);
    strlcat(dst, q, cap);
    (*n)++;
}

void jash_world_diff(jash_world_scan_t* old_scan,
                     const jash_world_scan_t* new_scan,
                     jash_world_delta_t* delta) {
    memset(delta, 0, sizeof(*delta));
    if (!old_scan || !new_scan) return;

    /* Both scans are path-sorted. A merge identifies all changes in one pass,
     * avoiding the old per-entry allocation plus old×new deletion scan. */
    int old_i = 0, new_i = 0;
    while (old_i < old_scan->count || new_i < new_scan->count) {
        if (old_i >= old_scan->count) {
            wd_append(delta->created, sizeof(delta->created), &delta->n_created, &delta->total_created,
                      new_scan->entries[new_i++].path);
            continue;
        }
        if (new_i >= new_scan->count) {
            wd_append(delta->deleted, sizeof(delta->deleted), &delta->n_deleted, &delta->total_deleted,
                      old_scan->entries[old_i++].path);
            continue;
        }
        const jash_world_entry_t* oe = &old_scan->entries[old_i];
        const jash_world_entry_t* ne = &new_scan->entries[new_i];
        int cmp = strcmp(oe->path, ne->path);
        delta->path_comparisons++;
        if (cmp < 0) {
            wd_append(delta->deleted, sizeof(delta->deleted), &delta->n_deleted, &delta->total_deleted,
                      oe->path);
            old_i++;
        } else if (cmp > 0) {
            wd_append(delta->created, sizeof(delta->created), &delta->n_created, &delta->total_created,
                      ne->path);
            new_i++;
        } else {
            if (oe->mtime != ne->mtime || oe->size != ne->size)
                wd_append(delta->modified, sizeof(delta->modified),
                          &delta->n_modified, &delta->total_modified, ne->path);
            old_i++;
            new_i++;
        }
    }

    // Attribution: best-effort — check chain.log for commands near mtime.
    if (delta->n_modified > 0 || delta->n_created > 0 || delta->n_deleted > 0) {
        FILE* f = jash_chain_open();
        if (f) {
            jash_chain_entry_t ce;
            // Collect chain entries (newest first, last 64).
            jash_chain_entry_t chain[64];
            int nchain = 0;
            while (jash_chain_next(f, &ce) && nchain < 64)
                chain[nchain++] = ce;
            (void)fclose(f);

            // For each changed path, find the closest chain entry by mtime.
            // (Simplified: just attribute to the most recent chain entry.)
            if (nchain > 0) {
                // Use the most recent chain entry as attribution source.
                char tag[32];
                snprintf(tag, sizeof(tag), "%s", chain[0].tag);
                char by[64];
                snprintf(by, sizeof(by), "chain#%llu", chain[0].seq);
                jash_json_buffer_t buffer = {delta->attribution, sizeof(delta->attribution), 0};
                jash_json_writer_t writer;
                jash_jsonw_init(&writer, jash_jsonw_buffer_sink, &buffer);
                jash_jsonw_raw(&writer, "{\"method\":\"chain\",\"by\":");
                jash_jsonw_string(&writer, by);
                jash_jsonw_raw(&writer, ",\"tag\":"); jash_jsonw_string_or_bytes(&writer, tag);
                jash_jsonw_raw(&writer, ",\"inferred\":true}");
                if (jash_jsonw_finish(&writer) != 0) delta->attribution[0] = '\0';
            } else {
                snprintf(delta->attribution, sizeof(delta->attribution),
                         "\"external\"");
            }
        } else {
            snprintf(delta->attribution, sizeof(delta->attribution), "\"external\"");
        }
    }

}

static int world_session_valid(const char* id) {
    return id && *id && !strchr(id, '/') && strcmp(id, ".") && strcmp(id, "..");
}

void jash_world_store(const jash_world_scan_t* scan, const char* session_id) {
    if (!scan || !world_session_valid(session_id)) return;
    const char* home = getenv("HOME");
    if (!home || !*home) return;
    char dir[512], path[600];
    int n = snprintf(dir, sizeof(dir), "%s/.jash/world", home);
    if (n < 0 || (size_t)n >= sizeof(dir)) return;
    mkdir(dir, 0700);
    n = snprintf(path, sizeof(path), "%s/%s.scan", dir, session_id);
    if (n < 0 || (size_t)n >= sizeof(path)) return;
    char temporary[sizeof(path) + 16];
    snprintf(temporary, sizeof(temporary), "%s.XXXXXX", path);
    int fd = mkstemp(temporary);
    if (fd < 0) return;
    (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
    FILE* f = fdopen(fd, "w");
    if (!f) { close(fd); unlink(temporary); return; }
    int failed = fputs("#jash-world-v2\n", f) == EOF;
    for (int i = 0; i < scan->count; i++) {
        const jash_world_entry_t* e = &scan->entries[i];
        char encoded[2 * JASH_WORLD_PATH_MAX + 1];
        static const char hex[] = "0123456789abcdef";
        size_t len = strnlen(e->path, sizeof(e->path));
        if (!len || len == sizeof(e->path)) { failed = 1; break; }
        for (size_t j = 0; j < len; j++) {
            unsigned char c = (unsigned char)e->path[j];
            encoded[2*j] = hex[c >> 4]; encoded[2*j+1] = hex[c & 15];
        }
        encoded[2*len] = 0;
        if (fprintf(f, "%s\t%ld\t%ld\t%d\n", encoded, e->mtime, e->size, e->mode) < 0) {
            failed = 1; break;
        }
    }
    if (fclose(f) != 0) failed = 1;
    if (!failed && rename(temporary, path) == 0) return;
    (void)unlink(temporary);
}

static int world_hex(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static int world_record(char* line, int version, jash_world_entry_t* e) {
    size_t len = strlen(line);
    if (!len || line[len-1] != '\n') return -1;
    line[--len] = 0;
    char* fields[4] = {line};
    for (int i = 1; i < 4; i++) {
        char* tab = strchr(fields[i-1], '\t');
        if (!tab) return -1;
        *tab = 0; fields[i] = tab + 1;
    }
    if (strchr(fields[3], '\t')) return -1;
    size_t plen = strlen(fields[0]);
    if (version == 2) {
        if (!plen || plen % 2 || plen / 2 >= sizeof(e->path)) return -1;
        for (size_t i = 0; i < plen; i += 2) {
            int a = world_hex((unsigned char)fields[0][i]);
            int b = world_hex((unsigned char)fields[0][i+1]);
            if (a < 0 || b < 0 || (a == 0 && b == 0)) return -1;
            e->path[i/2] = (char)((a << 4) | b);
        }
        e->path[plen/2] = 0;
    } else {
        if (!plen || plen >= sizeof(e->path)) return -1;
        memcpy(e->path, fields[0], plen + 1);
    }
    long values[3];
    for (int i = 0; i < 3; i++) {
        char* end;
        errno = 0;
        values[i] = strtol(fields[i+1], &end, 10);
        if (errno || end == fields[i+1] || *end) return -1;
    }
    if (values[1] < 0 || values[2] < 0 || values[2] > INT_MAX) return -1;
    e->mtime = values[0]; e->size = values[1]; e->mode = (int)values[2];
    return 0;
}

jash_world_scan_t* jash_world_load(const char* session_id) {
    if (!world_session_valid(session_id)) return NULL;
    const char* home = getenv("HOME");
    if (!home || !*home) return NULL;
    char path[600];
    int pn = snprintf(path, sizeof(path), "%s/.jash/world/%s.scan", home, session_id);
    if (pn < 0 || (size_t)pn >= sizeof(path)) return NULL;
    FILE* f = fopen(path, "r");
    if (!f) return NULL;

    jash_world_scan_t* scan = (jash_world_scan_t*)malloc(sizeof(jash_world_scan_t));
    if (!scan) { (void)fclose(f); return NULL; }
    scan->capacity = 64;
    scan->entries = (jash_world_entry_t*)malloc((size_t)scan->capacity * sizeof(jash_world_entry_t));
    scan->count = 0;
    if (!scan->entries) { free(scan); (void)fclose(f); return NULL; }

    char line[2 * JASH_WORLD_PATH_MAX + 96];
    int first = 1, version = 1, failed = 0;
    while (fgets(line, sizeof(line), f)) {
        if (first) {
            first = 0;
            if (!strcmp(line, "#jash-world-v2\n")) { version = 2; continue; }
        }
        if (scan->count >= JASH_WORLD_MAX_ENTRIES) { failed = 1; break; }
        if (scan->count == scan->capacity) {
            int grown_capacity = scan->capacity * 2;
            if (grown_capacity > JASH_WORLD_MAX_ENTRIES)
                grown_capacity = JASH_WORLD_MAX_ENTRIES;
            jash_world_entry_t* grown = (jash_world_entry_t*)realloc(
                scan->entries, (size_t)grown_capacity * sizeof(*scan->entries));
            if (!grown) {
                free(scan->entries);
                free(scan);
                (void)fclose(f);
                return NULL;
            }
            scan->entries = grown;
            scan->capacity = grown_capacity;
        }
        jash_world_entry_t* e = &scan->entries[scan->count];
        if (world_record(line, version, e) != 0) { failed = 1; break; }
        scan->count++;
    }
    if (ferror(f)) failed = 1;
    if (fclose(f) != 0) failed = 1;
    if (failed) { jash_world_scan_free(scan); return NULL; }
    qsort(scan->entries, (size_t)scan->count, sizeof(*scan->entries),
          entry_path_cmp);
    for (int i = 1; i < scan->count; i++) {
        if (!strcmp(scan->entries[i-1].path, scan->entries[i].path)) {
            jash_world_scan_free(scan); return NULL;
        }
    }
    return scan;
}

void jash_world_scan_free(jash_world_scan_t* scan) {
    if (scan) { free(scan->entries); free(scan); }
}

int jash_world_delta_json(const jash_world_delta_t* delta, char* buf, int buflen) {
    if (!delta || !buf || buflen <= 0) return 0;
    buf[0] = '\0';
    jash_json_buffer_t buffer = {buf, (size_t)buflen, 0};
    jash_json_writer_t writer;
    jash_jsonw_init(&writer, jash_jsonw_buffer_sink, &buffer);
    jash_world_delta_write(delta, &writer);
    if (jash_jsonw_finish(&writer) != 0) { buf[0] = '\0'; return -1; }
    return (int)buffer.used;
}

void jash_world_delta_write(const jash_world_delta_t* delta, jash_json_writer_t* w) {
    jash_jsonw_raw(w, "\"world_delta\":{\"since_s\":");
    jash_jsonw_safe_i64(w, delta->since_s);
    jash_jsonw_raw(w, ",\"created\":["); jash_jsonw_raw(w, delta->created);
    jash_jsonw_raw(w, "],\"modified\":["); jash_jsonw_raw(w, delta->modified);
    jash_jsonw_raw(w, "],\"deleted\":["); jash_jsonw_raw(w, delta->deleted);
    jash_jsonw_raw(w, "],\"total_created\":"); jash_jsonw_i64(w, delta->total_created);
    jash_jsonw_raw(w, ",\"total_modified\":"); jash_jsonw_i64(w, delta->total_modified);
    jash_jsonw_raw(w, ",\"total_deleted\":"); jash_jsonw_i64(w, delta->total_deleted);
    int truncated = delta->total_created > delta->n_created ||
                    delta->total_modified > delta->n_modified || delta->total_deleted > delta->n_deleted;
    jash_jsonw_raw(w, truncated ? ",\"truncated\":true" : ",\"truncated\":false");
    jash_jsonw_raw(w, ",\"scope\":\"bounded-metadata-snapshots\"");
    if (delta->attribution[0]) {
        jash_jsonw_raw(w, ",\"attribution\":"); jash_jsonw_raw(w, delta->attribution);
    }
    jash_jsonw_raw(w, "}");
}

jash_world_delta_t* jash_world_check_with_scan(const char* session_id,
                                               const char* cwd,
                                               jash_world_scan_t* new_scan) {
    if (!new_scan) return NULL;
    if (!session_id || !*session_id || !cwd || !*cwd) {
        jash_world_scan_free(new_scan);
        return NULL;
    }
    const char* env = getenv("JASH_WORLD");
    if (env && env[0] == '0') {
        jash_world_scan_free(new_scan);
        return NULL;
    }

    // Load previous snapshot.
    jash_world_scan_t* old_scan = jash_world_load(session_id);

    // Compute time since last scan.
    long now = (long)time(NULL);
    long since = 0;

    jash_world_delta_t* delta = NULL;
    if (old_scan) {
        // Compute since from the newest mtime in old scan.
        long newest = 0;
        for (int i = 0; i < old_scan->count; i++)
            if (old_scan->entries[i].mtime > newest)
                newest = old_scan->entries[i].mtime;
        since = now - newest;
        if (since < 0) since = 0;

        delta = (jash_world_delta_t*)malloc(sizeof(jash_world_delta_t));
        if (delta) {
            jash_world_diff(old_scan, new_scan, delta);
            delta->since_s = since;
        }
        jash_world_scan_free(old_scan);
    }

    // Store current snapshot for next invocation.
    jash_world_store(new_scan, session_id);
    jash_world_scan_free(new_scan);

    // Return delta only if there are changes.
    if (delta && delta->n_created == 0 && delta->n_modified == 0 && delta->n_deleted == 0) {
        free(delta);
        return NULL;
    }
    return delta;
}

jash_world_delta_t* jash_world_check(const char* session_id, const char* cwd) {
    if (!session_id || !*session_id || !cwd || !*cwd) return NULL;
    const char* env = getenv("JASH_WORLD");
    if (env && env[0] == '0') return NULL;
    int max_depth = 3;
    const char* denv = getenv("JASH_WORLD_DEPTH");
    if (denv && *denv) max_depth = atoi(denv);
    if (max_depth < 1) max_depth = 1;
    if (max_depth > 5) max_depth = 5;
    return jash_world_check_with_scan(session_id, cwd,
                                      jash_world_scan(cwd, max_depth));
}
