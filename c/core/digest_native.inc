// SPDX-License-Identifier: LicenseRef-SPL-1.0
/* The project reader/hash/classifier above is shared with the text engine.
 * This adapter retains its real observations before any publication. */
#include "digest_native.h"
#include <sys/file.h>

/* Store contention has a finite bound even in standalone invocations. This is
 * a lock deadline, not a claim that a cancellation request was observed. */
static int dn_lock(int fd) {
    struct timespec started, now;
    if (clock_gettime(CLOCK_MONOTONIC, &started)) return -1;
    for (;;) {
        if (!flock(fd, LOCK_EX | LOCK_NB)) return 0;
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) return -1;
        if (clock_gettime(CLOCK_MONOTONIC, &now)) return -1;
        int64_t elapsed = (int64_t)(now.tv_sec - started.tv_sec) * 1000 +
            (now.tv_nsec - started.tv_nsec) / 1000000;
        if (elapsed >= 5000) { errno = ETIMEDOUT; return -1; }
        struct timespec pause = {0, 10000000};
        while (nanosleep(&pause, &pause) && errno == EINTR) {}
    }
}

typedef struct { const digest_entry_t* entry; const digest_entry_t* old; const char* change; } dn_row_t;
struct digest_snapshot {
    digest_cfg_t config;
    digest_inventory_t inventory;
    digest_base_t base;
    digest_buffer_t briefing;
    digest_native_observation_t observed;
    dn_row_t* rows;
    char* root;
};

static int dn_read_fd(int fd, size_t maximum, int private_file, char** bytes, size_t* length) {
    *bytes = NULL; *length = 0;
    struct stat before, after;
    int error = 0;
    if (fstat(fd, &before)) error = errno;
    else if (!S_ISREG(before.st_mode) || (private_file && !digest_cache_safe(&before))) error = EPERM;
    else if (before.st_size < 0 || (uint64_t)before.st_size > maximum) error = EFBIG;
    size_t size = error ? 0 : (size_t)before.st_size;
    char* data = error ? NULL : malloc(size + 1);
    if (!error && !data) error = ENOMEM;
    size_t used = 0;
    while (!error) {
        ssize_t got = read(fd, data + used, size + 1 - used);
        if (got < 0 && errno == EINTR) continue;
        if (got < 0) { error = errno; break; }
        if (!got) break;
        used += (size_t)got;
        if (used > size) { error = ESTALE; break; }
    }
    if (!error && (used != size || fstat(fd, &after) || !digest_same_stat(&before, &after))) error = ESTALE;
    if (close(fd) && !error) error = errno;
    if (error) { free(data); errno = error; return -1; }
    data[used] = 0; *bytes = data; *length = used; return 0;
}

static int dn_read_at(int dir, const char* name, size_t maximum, int private_file,
                      char** bytes, size_t* length) {
    int fd = openat(dir, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    return fd < 0 ? -1 : dn_read_fd(fd, maximum, private_file, bytes, length);
}

static int dn_open_relative(int root, const char* path) {
    if (!path || !*path || path[0] == '/' || strlen(path) >= DIGEST_MAX_PATH) { errno = EINVAL; return -1; }
    char copy[DIGEST_MAX_PATH]; strcpy(copy, path);
    int dir = dup(root); if (dir < 0) return -1;
    char* part = copy;
    for (;;) {
        char* slash = strchr(part, '/');
        if (slash) *slash = 0;
        if (!*part || !strcmp(part, ".") || !strcmp(part, "..")) { close(dir); errno = EINVAL; return -1; }
        int fd = openat(dir, part, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC | (slash ? O_DIRECTORY : 0));
        int saved = errno; close(dir);
        if (fd < 0) { errno = saved; return -1; }
        if (!slash) return fd;
        dir = fd; part = slash + 1;
    }
}

const char* digest_native_ignored_directory(size_t index) {
    size_t count = sizeof digest_skipped_dirs / sizeof digest_skipped_dirs[0];
    return index < count ? digest_skipped_dirs[index] : NULL;
}

void digest_snapshot_close(digest_snapshot_t* snapshot) {
    if (!snapshot) return;
    for (size_t i = 0; i < snapshot->inventory.count; i++) {
        free(snapshot->inventory.entries[i].path); free(snapshot->inventory.entries[i].capture);
    }
    free(snapshot->inventory.entries); digest_base_free(&snapshot->base);
    free(snapshot->briefing.data); free(snapshot->rows); free(snapshot->root); free(snapshot);
}

int digest_snapshot_prepare(const digest_cfg_t* cfg, digest_snapshot_t** output) {
    *output = NULL;
    if (!cfg || !cfg->root_dir || cfg->level < DIGEST_BRIEF || cfg->level > DIGEST_DEEP ||
        cfg->jobs < 0 || cfg->jobs > DIGEST_MAX_JOBS || cfg->fetch_hash ||
        (cfg->fingerprint && cfg->manifest) || (cfg->diff_base && (cfg->fingerprint || cfg->manifest))) {
        errno = EINVAL; return -1;
    }
    digest_snapshot_t* snapshot = calloc(1, sizeof *snapshot);
    if (!snapshot) return -1;
    snapshot->config = *cfg;
    snapshot->root = realpath(cfg->root_dir, NULL);
    if (!snapshot->root) goto failed;
    int root = digest_open_dir(snapshot->root); if (root < 0) goto failed;
    digest_inventory_t* inv = &snapshot->inventory; inv->readme_rank = 100;
    inv->entries = calloc(DIGEST_MAX_ENTRIES, sizeof *inv->entries);
    if (!inv->entries) { close(root); goto failed; }
    digest_pool_t pool;
    int workers = cfg->jobs ? cfg->jobs : (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (workers < 1) workers = 1;
    if (!cfg->jobs && workers > 8) workers = 8;
    digest_pool_init(&pool, workers);
    int walked = digest_walk(root, "", 0, inv, &pool);
    digest_pool_finish(&pool, walked);
    if (walked || digest_inventory_settle(inv)) goto failed;
    digest_native_observation_t* obs = &snapshot->observed;
    obs->root = snapshot->root; obs->level = digest_level_name(cfg->level);
    digest_fingerprint(inv, snapshot->root, obs->fingerprint);
    obs->visited = inv->visited; obs->files = inv->count - inv->counts[D_DIR];
    obs->directories = inv->counts[D_DIR]; obs->bytes = inv->bytes; obs->lines = inv->lines;
    obs->excluded_files = inv->excluded_files; obs->excluded_special = inv->excluded_special;
    obs->excluded_directories = inv->excluded_directories;
    for (size_t i = 0; i < 7; i++) obs->categories[i] = inv->counts[i];
    for (size_t i = 0; i < 11; i++) obs->languages[i] = inv->languages[i];
    obs->readme_path = inv->readme_path; obs->readme = inv->readme; obs->readme_bytes = inv->readme_len;
    if (cfg->diff_base) {
        char* bytes = NULL; size_t length = 0;
        if (dn_read_at(AT_FDCWD, cfg->diff_base, DIGEST_DIFF_MAX_BASE_BYTES, 0, &bytes, &length)) goto failed;
        int parsed = digest_manifest_parse(bytes, length, &snapshot->base); free(bytes);
        if (parsed) { errno = EINVAL; goto failed; }
        strcpy(obs->base_fingerprint, snapshot->base.base_fp);
    }
    size_t capacity = inv->count + snapshot->base.count;
    snapshot->rows = calloc(capacity ? capacity : 1, sizeof *snapshot->rows);
    if (!snapshot->rows) goto failed;
    for (size_t i = 0; i < inv->count; i++) {
        digest_entry_t* entry = &inv->entries[i];
        if (!cfg->diff_base) snapshot->rows[obs->entries++] = (dn_row_t){entry, NULL, NULL};
        else {
            digest_entry_t* old = bsearch(entry->path, snapshot->base.entries, snapshot->base.count,
                                          sizeof *snapshot->base.entries, digest_path_cmp);
            if (!old || old->kind != entry->kind) snapshot->rows[obs->entries++] = (dn_row_t){entry, NULL, "added"};
            else if (entry->kind != D_DIR && memcmp(old->hash, entry->hash, sizeof entry->hash))
                snapshot->rows[obs->entries++] = (dn_row_t){entry, old, "modified"};
        }
    }
    for (size_t i = 0; i < snapshot->base.count; i++) {
        digest_entry_t* old = &snapshot->base.entries[i];
        digest_entry_t* live = bsearch(old->path, inv->entries, inv->count, sizeof *inv->entries, digest_path_cmp);
        if (!live || live->kind != old->kind) snapshot->rows[obs->entries++] = (dn_row_t){old, NULL, "removed"};
    }
    obs->cache_requested = !cfg->no_cache && !cfg->manifest && !cfg->fingerprint && !cfg->diff_base;
    if (obs->cache_requested) {
        struct blake3 hash; unsigned char digest[BLAKE3_OUT_LEN];
        blake3_init(&hash); digest_hash_field(&hash, obs->fingerprint, 64);
        digest_hash_field(&hash, obs->level, strlen(obs->level));
        blake3_final(&hash, digest, sizeof digest); digest_hash_hex(digest, obs->cache_key);
        int cache = digest_cache_open_ro();
        if (cache < 0 && errno != ENOENT) goto failed;
        if (cache >= 0) {
            char name[80]; snprintf(name, sizeof name, "%s.md", obs->cache_key);
            int result = digest_cache_target(cache, name);
            if (!result && !cfg->refresh) {
                result = digest_cache_read(cache, name, obs->cache_key, &snapshot->briefing);
                obs->cache_hit = result == 0;
            }
            int saved = errno; if (close(cache) && result >= 0) result = -1; else errno = saved;
            if (result < 0) goto failed;
        }
        if (!obs->cache_hit && digest_compose(cfg, obs->root, inv, obs->fingerprint, &snapshot->briefing)) goto failed;
    }
    *output = snapshot; return 0;
failed:
    { int saved = errno ? errno : EIO; digest_snapshot_close(snapshot); errno = saved; return -1; }
}

const digest_native_observation_t* digest_snapshot_observation(const digest_snapshot_t* snapshot) {
    return snapshot ? &snapshot->observed : NULL;
}

int digest_snapshot_entry(const digest_snapshot_t* snapshot, size_t index, digest_native_entry_t* output) {
    if (!snapshot || index >= snapshot->observed.entries || !output) return -1;
    const dn_row_t* row = &snapshot->rows[index]; const digest_entry_t* entry = row->entry;
    memset(output, 0, sizeof *output);
    output->path = entry->path; output->kind = digest_kind_names[entry->kind];
    output->language = entry->language >= 0 && entry->language < L_NONE ? digest_languages[entry->language] : NULL;
    output->change = row->change; output->directory = entry->kind == D_DIR;
    output->bytes = entry->bytes; output->lines = entry->lines;
    if (!output->directory) digest_hash_hex(entry->hash, output->blake3);
    if (row->old) digest_hash_hex(row->old->hash, output->old_blake3);
    return 0;
}

/* Publication records rename and directory fsync separately. */
static int dn_publish_at(int dir, const char* name, const char* bytes, size_t length,
                          int* published, int* durable) {
    *published = *durable = 0;
    if (digest_cache_target(dir, name)) return -1;
    static _Atomic unsigned serial;
    char temporary[96]; int fd = -1;
    for (int i = 0; i < 128; i++) {
        snprintf(temporary, sizeof temporary, ".native-%ld-%u.tmp", (long)getpid(), atomic_fetch_add(&serial, 1));
        fd = openat(dir, temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd >= 0 || errno != EEXIST) break;
    }
    if (fd < 0) return -1;
    int failed = digest_write_all(fd, bytes, length) || fsync(fd);
    int saved = errno;
    if (close(fd) && !failed) { failed = 1; saved = errno; }
    if (!failed && digest_cache_target(dir, name)) { failed = 1; saved = errno; }
    if (!failed && renameat(dir, temporary, dir, name)) { failed = 1; saved = errno; }
    if (!failed) {
        *published = 1;
        if (fsync(dir)) { failed = 1; saved = errno; } else *durable = 1;
    }
    if (failed) { unlinkat(dir, temporary, 0); errno = saved ? saved : EIO; return -1; }
    return 0;
}

static int dn_populate_cas(digest_snapshot_t* snapshot, int cache) {
    digest_native_observation_t* obs = &snapshot->observed;
    int cas = -1, tmp = -1, root = -1, status = -1;
    obs->cas_attempted = 1;
    if (digest_cas_setup(cache, &cas, &tmp)) goto done;
    if (dn_lock(cas)) goto done;
    uint64_t total = digest_cas_snapshot(cas);
    if (total > DIGEST_CAS_MAX_BYTES) { errno = EFBIG; goto done; }
    root = digest_open_dir(snapshot->root); if (root < 0) goto done;
    for (size_t i = 0; i < snapshot->inventory.count; i++) {
        digest_entry_t* entry = &snapshot->inventory.entries[i];
        if (entry->kind == D_DIR) continue;
        char hash[65]; digest_hash_hex(entry->hash, hash);
        char shard[3] = {hash[0], hash[1], 0};
        int dir = digest_private_dir(cas, shard); if (dir < 0) goto done;
        char *content = NULL, *existing = NULL; size_t length = 0, old_length = 0;
        int fd = dn_open_relative(root, entry->path);
        if (fd < 0 || dn_read_fd(fd, DIGEST_MAX_FILE_BYTES, 0, &content, &length)) { close(dir); goto done; }
        char actual[65]; digest_bytes_hash(content, length, actual);
        if (length != entry->bytes || strcmp(actual, hash)) {
            free(content); close(dir); errno = ESTALE; goto done;
        }
        int read = dn_read_at(dir, hash + 2, DIGEST_MAX_FILE_BYTES, 1, &existing, &old_length);
        int missing = read && errno == ENOENT;
        int valid = !read && old_length == length && !memcmp(content, existing, length);
        free(existing);
        if (read && !missing) { free(content); close(dir); goto done; }
        if (!valid) {
            uint64_t growth = length > old_length ? length - old_length : 0;
            if (growth > DIGEST_CAS_MAX_BYTES - total) {
                free(content); close(dir); errno = EFBIG; goto done;
            }
            int published, durable;
            if (dn_publish_at(dir, hash + 2, content, length, &published, &durable)) {
                free(content); close(dir); goto done;
            }
            total = total - old_length + length;
        }
        free(content);
        if (close(dir)) goto done;
        obs->cas_verified_objects++;
    }
    status = 0; obs->cas_complete = 1;
done:
    { int saved = errno;
      if (root >= 0 && close(root) && !status) { status = -1; saved = errno; }
      if (tmp >= 0 && close(tmp) && !status) { status = -1; saved = errno; }
      if (cas >= 0 && close(cas) && !status) { status = -1; saved = errno; }
      if (status) obs->cas_complete = 0;
      errno = saved; return status; }
}

static int dn_ledger_append(digest_snapshot_t* snapshot, int cache) {
    digest_native_observation_t* obs = &snapshot->observed;
    int fd = openat(cache, "ledger.jsonl", O_RDWR | O_CREAT | O_APPEND | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    struct stat st; int error = 0;
    if (dn_lock(fd) || fstat(fd, &st) || !digest_cache_safe(&st)) error = errno ? errno : EPERM;
    digest_buffer_t body = {0}; jash_json_writer_t json;
    jash_jsonw_init(&json, digest_buffer_writer, &body);
    time_t now = time(NULL);
    jash_jsonw_raw(&json, "{\"schema\":\"jash.digest-receipt.v1\",\"generator\":");
    jash_jsonw_string(&json, DIGEST_GENERATOR_VERSION);
    jash_jsonw_raw(&json, ",\"ts\":"); jash_jsonw_u64(&json, now < 0 ? 0 : (uint64_t)now);
    jash_jsonw_raw(&json, ",\"root\":"); jash_jsonw_string_or_bytes(&json, obs->root);
    jash_jsonw_raw(&json, ",\"level\":"); jash_jsonw_string(&json, obs->level);
    jash_jsonw_raw(&json, ",\"fingerprint\":"); jash_jsonw_string(&json, obs->fingerprint);
    jash_jsonw_raw(&json, ",\"served_from_cache\":"); jash_jsonw_raw(&json, obs->cache_hit ? "true" : "false");
    jash_jsonw_raw(&json, ",\"bytes_out\":"); jash_jsonw_u64(&json, snapshot->briefing.len);
    jash_jsonw_raw(&json, ",\"files\":"); jash_jsonw_u64(&json, obs->files);
    jash_jsonw_raw(&json, ",\"directories\":"); jash_jsonw_u64(&json, obs->directories);
    jash_jsonw_raw(&json, ",\"lines\":"); jash_jsonw_u64(&json, obs->lines);
    jash_jsonw_raw(&json, "}\n");
    if (!error && (jash_jsonw_finish(&json) || body.failed)) error = E2BIG;
    if (!error && (st.st_size < 0 || (uint64_t)st.st_size > DIGEST_LEDGER_READ_MAX - body.len)) error = EFBIG;
    if (!error && digest_write_all(fd, body.data, body.len)) error = errno ? errno : EIO;
    if (!error) { obs->ledger_written = 1; if (fsync(fd)) error = errno; }
    if (flock(fd, LOCK_UN) && !error) error = errno;
    if (close(fd) && !error) error = errno;
    if (!error && fsync(cache)) error = errno;
    free(body.data); obs->ledger_durable = !error;
    if (error) { errno = error; return -1; } return 0;
}

int digest_snapshot_apply(digest_snapshot_t* snapshot) {
    if (!snapshot) { errno = EINVAL; return -1; }
    digest_native_observation_t* obs = &snapshot->observed;
    if (!obs->cache_requested) return 0;
    obs->effects_started = 1;
    int cache = digest_cache_open(); if (cache < 0) { obs->error = "cache_open_failed"; return -1; }
    if (!obs->cache_hit) {
        digest_buffer_t file = {0}; char hash[65], name[80];
        digest_bytes_hash(snapshot->briefing.data, snapshot->briefing.len, hash);
        snprintf(name, sizeof name, "%s.md", obs->cache_key);
        jash_writef(digest_buffer_writer, &file, "<!-- %s %s %lu %s -->\n",
            DIGEST_GENERATOR_VERSION, obs->cache_key, (unsigned long)snapshot->briefing.len, hash);
        digest_buffer_writer(&file, snapshot->briefing.data, snapshot->briefing.len);
        int failed = file.failed || dn_publish_at(cache, name, file.data, file.len,
                                                    &obs->cache_published, &obs->cache_durable);
        free(file.data);
        if (failed) obs->error = "cache_publication_failed";
    }
    if (!obs->error && dn_populate_cas(snapshot, cache)) obs->error = "content_store_incomplete";
    if (!obs->error && dn_ledger_append(snapshot, cache)) obs->error = "ledger_append_failed";
    int saved = errno;
    if (close(cache) && !obs->error) { obs->error = "cache_close_failed"; saved = errno; }
    errno = saved; return obs->error ? -1 : 0;
}

int digest_native_ledger(char** bytes, size_t* length, int* present) {
    *bytes = NULL; *length = 0; *present = 0;
    int cache = digest_cache_open_ro();
    if (cache < 0) return errno == ENOENT ? 0 : -1;
    int result = dn_read_at(cache, "ledger.jsonl", DIGEST_LEDGER_READ_MAX, 1, bytes, length);
    if (result && errno == ENOENT) result = 0; else if (!result) *present = 1;
    int saved = errno; if (close(cache) && !result) return -1;
    errno = saved; return result;
}

int digest_native_fetch(const char* hash, char** bytes, size_t* length) {
    *bytes = NULL; *length = 0;
    if (!hash || strlen(hash) != 64 || strspn(hash, "0123456789abcdef") != 64) { errno = EINVAL; return -1; }
    int cache = digest_cache_open_ro(); if (cache < 0) return -1;
    int cas = openat(cache, "cas", O_RDONLY | O_NOFOLLOW | O_DIRECTORY | O_CLOEXEC);
    int saved = errno;
    if (close(cache) && cas >= 0) { saved = errno; close(cas); errno = saved; return -1; }
    errno = saved; if (cas < 0) return -1;
    char prefix[3] = {hash[0], hash[1], 0};
    int shard = openat(cas, prefix, O_RDONLY | O_NOFOLLOW | O_DIRECTORY | O_CLOEXEC);
    saved = errno;
    if (close(cas) && shard >= 0) { saved = errno; close(shard); errno = saved; return -1; }
    errno = saved; if (shard < 0) return -1;
    int result = dn_read_at(shard, hash + 2, DIGEST_MAX_FILE_BYTES, 1, bytes, length);
    saved = errno;
    if (close(shard) && !result) { saved = errno; result = -1; }
    if (!result) {
        char actual[65]; digest_bytes_hash(*bytes, *length, actual);
        if (strcmp(actual, hash)) { result = -1; saved = EINVAL; }
    }
    if (result) { free(*bytes); *bytes = NULL; *length = 0; }
    errno = saved; return result;
}

int digest_native_cache_status(digest_native_cache_t* observation) {
    memset(observation, 0, sizeof *observation);
    int cache = digest_cache_open_ro(); if (cache < 0) return errno == ENOENT ? 0 : -1;
    observation->present = 1;
    DIR* directory = fdopendir(cache); if (!directory) { close(cache); return -1; }
    int error = 0;
    for (;;) {
        errno = 0; struct dirent* entry = readdir(directory);
        if (!entry) { error = errno; break; }
        if (entry->d_name[0] == '.') continue;
        if (++observation->visited > DIGEST_CAS_SNAPSHOT_ENTRIES) { error = E2BIG; break; }
        size_t n = strlen(entry->d_name);
        if (n < 4 || strcmp(entry->d_name + n - 3, ".md")) continue;
        struct stat st;
        if (fstatat(cache, entry->d_name, &st, AT_SYMLINK_NOFOLLOW)) { error = errno; break; }
        if (!digest_cache_safe(&st) || st.st_size < 0) { error = EPERM; break; }
        uint64_t size = (uint64_t)st.st_size;
        if (size > UINT64_MAX - observation->bytes) { error = EOVERFLOW; break; }
        observation->entries++; observation->bytes += size;
        uint64_t timestamp = st.st_mtime < 0 ? 0 : (uint64_t)st.st_mtime;
        if (!observation->oldest || timestamp < observation->oldest) observation->oldest = timestamp;
        if (timestamp > observation->newest) observation->newest = timestamp;
    }
    if (closedir(directory) && !error) error = errno;
    if (error) { errno = error; return -1; } return 0;
}
