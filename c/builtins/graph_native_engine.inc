// SPDX-License-Identifier: LicenseRef-SPL-1.0
/* Checked storage and source observations for the same graph inventory,
 * extraction, resolution and community engines used by the text frontend. */
#include "memory_chain.h"
#include "gap_snapshot.h"
#include <core/query_cancel.h>
#include <module/jash_crypto.h>

#define GN_STORE_BYTES (32u * 1024u * 1024u)
#define GN_FILE_BYTES (1024u * 1024u)
#define GN_SOURCE_BYTES (128u * 1024u * 1024u)
#define GN_VISITS 65536u
typedef struct {
    jash_state_t* state;
    semantic_builtin_t* call;
    int source_root, error;
    uint64_t visited, source_bytes, excluded;
    char last_hash[65];
} gn_control_t;
static gn_control_t* gn_control;

static int gn_poll(void) {
    int status = jash_query_cancel_poll(gn_control->state);
    if (status) { gn_control->call->cancellation_status = status; errno = ECANCELED; }
    return status;
}

static int gn_names(char name[128], char lock[128], int mesh) {
    const char* key = jash_repo_key();
    int scoped = key && *key && strcmp(key, "-");
    int n = snprintf(name, 128, scoped ? "graph-%s.tsv" : "graph.tsv", key);
    if (n < 0 || n >= 128) { errno = ENAMETOOLONG; return -1; }
    n = mesh ? snprintf(lock, 128, ".lock") :
        snprintf(lock, 128, scoped ? "graph-%s.lock" : "graph.lock", key);
    if (n < 0 || n >= 128) { errno = ENAMETOOLONG; return -1; }
    return 0;
}

static int gn_store_close(memory_store_t* store) {
    int error = 0;
    if (store->lock >= 0) {
        if (flock(store->lock, LOCK_UN)) error = errno;
        if (close(store->lock) && !error) error = errno;
        store->lock = -1;
    }
    if (store->directory >= 0 && close(store->directory) && !error) error = errno;
    store->directory = -1;
    if (error) { errno = error; return -1; } return 0;
}

static int gn_store_open(memory_store_t* store, int mesh, int write) {
    *store = (memory_store_t){-1, -1};
    const char* home = getenv("HOME");
    if (!home || home[0] != '/') { errno = EINVAL; return -1; }
    int directory = open(home, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (directory < 0) return -1;
    int child = -1, error = 0;
    if (memory_store_trusted(directory, 1)) error = errno;
    if (!error) { child = memory_store_child(directory, ".jash", write); if (child < 0) error = errno; }
    if (close(directory) && !error) error = errno;
    if (error) { if (child >= 0) close(child); if (!write && error == ENOENT) return 0; errno = error; return -1; }
    directory = child;
    if (mesh) {
        child = memory_store_child(directory, "mesh", 0);
        if (child < 0) error = errno;
        if (close(directory) && !error) error = errno;
        if (error) { if (child >= 0) close(child); if (!write && error == ENOENT) return 0; errno = error; return -1; }
        directory = child;
    }
    store->directory = directory;
    if (!write) return 0;
    char name[128], lock_name[128];
    if (gn_names(name, lock_name, mesh)) goto fail;
    store->lock = openat(directory, lock_name, O_RDWR | O_CREAT | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600);
    if (store->lock < 0 || memory_store_trusted(store->lock, 0)) goto fail;
    struct timespec start, now;
    if (clock_gettime(CLOCK_MONOTONIC, &start)) goto fail;
    for (;;) {
        if (gn_poll()) goto fail;
        if (!flock(store->lock, LOCK_EX | LOCK_NB)) return 0;
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) goto fail;
        if (clock_gettime(CLOCK_MONOTONIC, &now)) goto fail;
        if ((int64_t)(now.tv_sec - start.tv_sec) * 1000 + (now.tv_nsec - start.tv_nsec) / 1000000 >= 5000) {
            errno = ETIMEDOUT; goto fail;
        }
        struct timespec pause = {0, 10000000}; (void)nanosleep(&pause, NULL);
    }
fail:
    error = errno; (void)gn_store_close(store); errno = error; return -1;
}

static void gn_reindex(void) {
    lg_reset_hashes(); memset(fstamp, 0, LG_CAP_FILES * sizeof *fstamp);
    for (int i = 0; i < nF; i++) {
        unsigned bucket = lg_fnv(F[i].path) & (LG_HFILE - 1);
        fnext[i] = fhash[bucket]; fhash[bucket] = i;
    }
    for (int i = 0; i < nN; i++) {
        unsigned bucket = lg_fnv(N[i].name) & (LG_HNAME - 1);
        nnext[i] = nhash[bucket]; nhash[bucket] = i;
    }
    int count = nE; nE = 0;
    for (int i = 0; i < count; i++) {
        lg_edge_t edge = E[i]; (void)lg_edge_add((int)edge.src, (int)edge.dst, edge.rel, edge.conf);
    }
}

static int gn_read(memory_store_t* store, const char* name, int old, int* present,
                   struct stat* observed) {
    char* bytes = NULL; size_t length = 0;
    if (memory_store_read(store, name, GN_STORE_BYTES, &bytes, &length, present)) return -1;
    lg_node_t* nodes = old ? oN : N; lg_edge_t* edges = old ? oE : E; lg_file_t* files = old ? oF : F;
    int nn = 0, ne = 0, nf = 0, error = 0, phase = 0;
    if (length && (memchr(bytes, 0, length) || memchr(bytes, '\r', length) || bytes[length - 1] != '\n')) error = EINVAL;
    if (*present && observed && fstatat(store->directory, name, observed, AT_SYMLINK_NOFOLLOW)) error = errno;
    int* hash = calloc(LG_HFILE, sizeof *hash); int* next = calloc(LG_CAP_FILES, sizeof *next);
    if (!hash || !next) error = ENOMEM;
    if (hash) for (int i = 0; i < LG_HFILE; i++) hash[i] = -1;
    if (!error) {
        uint8_t hash_bytes[32]; jash_blake3(bytes ? bytes : "", length, hash_bytes);
        jash_blake3_hex(hash_bytes, gn_control->last_hash);
    }
    char* line = bytes;
    while (!error && line && *line) {
        if (gn_poll()) { error = ECANCELED; break; }
        char* end = strchr(line, '\n');
        if (!end || (size_t)(end - line) > 1600) { error = EINVAL; break; }
        *end = 0; char* fields[8]; int fields_count = lg_split(line, fields, 8);
        uint64_t a, b;
        if (!strcmp(fields[0], "F") && fields_count == 4 && !phase && nf < LG_CAP_FILES) {
            size_t size = strlen(fields[1]); unsigned bucket = lg_fnv(fields[1]) & (LG_HFILE - 1);
            if (!size || size >= sizeof files[nf].path || fields[1][0] == '/' ||
                memory_unsigned(fields[2], LONG_MAX, &a) || memory_unsigned(fields[3], LONG_MAX, &b)) { error = EINVAL; break; }
            for (int i = hash[bucket]; i >= 0; i = next[i]) if (!strcmp(files[i].path, fields[1])) error = EINVAL;
            if (error) break;
            strcpy(files[nf].path, fields[1]); files[nf].mtime = (long)a; files[nf].size = (long)b;
            next[nf] = hash[bucket]; hash[bucket] = nf++;
        } else if (!strcmp(fields[0], "N") && fields_count == 6 && phase < 2 && nn < LG_CAP_NODES) {
            phase = 1;
            if (!*fields[1] || strlen(fields[1]) >= sizeof nodes[nn].name || strlen(fields[2]) != 1 ||
                !strchr("FSETM", fields[2][0]) || memory_unsigned(fields[4], LONG_MAX, &a) ||
                memory_unsigned(fields[5], LG_CAP_NODES - 1, &b)) { error = EINVAL; break; }
            int file = -1; unsigned bucket = lg_fnv(fields[3]) & (LG_HFILE - 1);
            for (int i = hash[bucket]; i >= 0; i = next[i]) if (!strcmp(files[i].path, fields[3])) { file = i; break; }
            if (file < 0) { error = EINVAL; break; }
            memset(&nodes[nn], 0, sizeof nodes[nn]); strcpy(nodes[nn].name, fields[1]);
            nodes[nn].type = fields[2][0]; nodes[nn].fidx = file;
            nodes[nn].line = (long)a; nodes[nn++].community = (int)b;
        } else if (!strcmp(fields[0], "E") && fields_count == 5 && ne < LG_CAP_EDGES) {
            phase = 2;
            if (!nn || memory_unsigned(fields[1], (uint64_t)nn - 1, &a) ||
                memory_unsigned(fields[2], (uint64_t)nn - 1, &b) || strlen(fields[3]) != 1 ||
                !strchr("CKI", fields[3][0]) || strlen(fields[4]) != 1 || !strchr("EI", fields[4][0])) { error = EINVAL; break; }
            edges[ne++] = (lg_edge_t){(unsigned)a, (unsigned)b, fields[3][0], fields[4][0]};
        } else { error = EINVAL; break; }
        line = end + 1;
    }
    for (int i = 0; !error && i < nn; i++) if (nodes[i].community >= nn) error = EINVAL;
    free(bytes); free(hash); free(next);
    if (error) { errno = error; return -1; }
    if (old) { onN = nn; onE = ne; onF = nf; }
    else { nN = nn; nE = ne; nF = nf; gn_reindex(); }
    return 0;
}

static memory_write_t gn_publish(memory_store_t* store, const char* name) {
    memory_write_t result = {0};
    jash_structured_buffer_t bytes; jash_structured_buffer_init(&bytes, GN_STORE_BYTES);
    for (int i = 0; i < nF; i++) jash_writef(jash_structured_buffer_sink, &bytes, "F\t%s\t%ld\t%ld\n", F[i].path, F[i].mtime, F[i].size);
    for (int i = 0; i < nN; i++) jash_writef(jash_structured_buffer_sink, &bytes, "N\t%s\t%c\t%s\t%ld\t%d\n",
        N[i].name, N[i].type, F[N[i].fidx].path, N[i].line, N[i].community);
    for (int i = 0; i < nE; i++) jash_writef(jash_structured_buffer_sink, &bytes, "E\t%u\t%u\t%c\t%c\n", E[i].src, E[i].dst, E[i].rel, E[i].conf);
    if (bytes.error || bytes.required > bytes.limit) { result.error = E2BIG; goto done; }
    if (gn_poll()) { result.error = ECANCELED; goto done; }
    char temporary[80]; static unsigned serial; int fd = -1;
    for (unsigned i = 0; i < 32; i++) {
        snprintf(temporary, sizeof temporary, ".graph-%ld-%u.tmp", (long)getpid(), ++serial);
        fd = openat(store->directory, temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd >= 0 || errno != EEXIST) break;
    }
    if (fd < 0) { result.error = errno; goto done; }
    if (memory_store_write_all(fd, bytes.data, bytes.used) || fsync(fd)) result.error = errno;
    if (close(fd) && !result.error) result.error = errno;
    if (!result.error && gn_poll()) result.error = ECANCELED;
    if (!result.error && renameat(store->directory, temporary, store->directory, name)) result.error = errno;
    else if (!result.error) {
        result.applied = 1;
        if (fsync(store->directory)) result.error = errno; else result.durable = 1;
    }
    if (!result.applied) (void)unlinkat(store->directory, temporary, 0);
done:
    jash_structured_buffer_free(&bytes); return result;
}

static int gn_walk_at(int directory, const char* relative, unsigned depth) {
    if (depth > 32 || gn_poll()) { errno = depth > 32 ? E2BIG : ECANCELED; return -1; }
    int fd = openat(directory, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return -1;
    DIR* stream = fdopendir(fd); if (!stream) { close(fd); return -1; }
    int error = 0;
    for (;;) {
        errno = 0; struct dirent* entry = readdir(stream);
        if (!entry) { error = errno; break; }
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        if (++gn_control->visited > GN_VISITS || gn_poll()) { error = errno == ECANCELED ? ECANCELED : E2BIG; break; }
        char path[384]; int length = snprintf(path, sizeof path, "%s%s%s", relative, *relative ? "/" : "", entry->d_name);
        if (length < 0 || (size_t)length >= sizeof path || strpbrk(path, "\t\r\n")) { error = ENAMETOOLONG; break; }
        struct stat st;
        if (fstatat(directory, entry->d_name, &st, AT_SYMLINK_NOFOLLOW)) { error = errno; break; }
        if (S_ISDIR(st.st_mode)) {
            if (lg_skip_dir(entry->d_name)) { gn_control->excluded++; continue; }
            int child = openat(directory, entry->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (child < 0) { error = errno; break; }
            if (gn_walk_at(child, path, depth + 1)) error = errno;
            if (close(child) && !error) error = errno;
            if (error) break;
        } else if (S_ISREG(st.st_mode)) {
            size_t n = strlen(entry->d_name);
            if (n < 3 || (strcmp(entry->d_name + n - 2, ".c") && strcmp(entry->d_name + n - 2, ".h"))) { gn_control->excluded++; continue; }
            if (st.st_size < 0 || (uint64_t)st.st_size > GN_FILE_BYTES || st.st_mtime < 0 ||
                (uint64_t)st.st_size > GN_SOURCE_BYTES - gn_control->source_bytes) { error = E2BIG; break; }
            int index = lg_file_add(path); if (index < 0) { error = E2BIG; break; }
            F[index].mtime = (long)st.st_mtime; F[index].size = (long)st.st_size;
            gn_control->source_bytes += (uint64_t)st.st_size;
        } else gn_control->excluded++;
    }
    if (closedir(stream) && !error) error = errno;
    if (error) { gn_control->error = error; errno = error; return -1; } return 0;
}

static int gn_walk(const char* root, const char* relative) {
    (void)root; return gn_walk_at(gn_control->source_root, relative, 0);
}

static int gn_extract(const char* root, int index) {
    (void)root;
    char path[384]; strcpy(path, F[index].path);
    int directory = dup(gn_control->source_root), fd = -1, error = 0;
    if (directory < 0) return 0;
    for (char* part = path; part && *part;) {
        char* slash = strchr(part, '/'); if (slash) *slash = 0;
        fd = openat(directory, part, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC | (slash ? O_DIRECTORY : 0));
        error = fd < 0 ? errno : 0;
        if (close(directory) && !error) error = errno;
        if (error) { if (fd >= 0) close(fd); gn_control->error = error; return 0; }
        if (!slash) break;
        directory = fd; part = slash + 1;
    }
    struct stat before, after;
    if (fstat(fd, &before)) error = errno;
    if (!error && (!S_ISREG(before.st_mode) || before.st_size != F[index].size || before.st_mtime != F[index].mtime)) error = ESTALE;
    size_t length = error ? 0 : (size_t)before.st_size;
    char* bytes = error ? NULL : calloc(length + 1, 1); if (!error && !bytes) error = ENOMEM;
    size_t used = 0;
    while (!error && used < length) {
        if (gn_poll()) { error = ECANCELED; break; }
        ssize_t n = read(fd, bytes + used, length - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { error = n < 0 ? errno : ESTALE; break; }
        used += (size_t)n;
    }
    if (!error && (fstat(fd, &after) || !memory_store_same(&before, &after))) error = ESTALE;
    if (close(fd) && !error) error = errno;
    if (!error && memchr(bytes, 0, length)) error = EILSEQ;
    JSH_ParseContext parser; memset(&parser, 0, sizeof parser);
    if (!error && (jsh_parse_init(&parser, NULL, F[index].path) || jsh_parse_string(&parser, bytes, length))) error = EINVAL;
    if (!error) {
        int module = lg_node_add(F[index].path, index, 0, 'M');
        if (module >= 0) fmodule[index] = module;
        lg_extract_nodes(index, module, &parser);
        for (uint32_t i = 0; i < parser.node_count; i++) if (parser.nodes[i].name.length >= 120) lg_truncated = 1;
    }
    jsh_parse_destroy(&parser); free(bytes);
    if (error) gn_control->error = error;
    return !error;
}
