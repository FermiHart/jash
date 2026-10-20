// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — core/digest_engine — observed project inventory and session briefings.
// Project mode is pure C: no subprocess, model, network, or inferred risks.

#define _GNU_SOURCE
#include "digest_engine.h"
#include <core/json_writer.h>
#include <module/blake3.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/syscalls.h> /* session digest's portable strlcpy */
#ifndef JASH_TARGET_BEYOND
#include <pthread.h>
#endif

enum { D_SOURCE, D_HEADER, D_TEST, D_DOC, D_BUILD, D_CONFIG, D_DIR, D_CLASSES };
enum { L_C, L_CPP, L_PYTHON, L_JS, L_TS, L_SHELL, L_RUST, L_HTML, L_CSS, L_TEMPLATE, L_SCRIPT, L_NONE };
static const char* const digest_languages[] = {
    "C", "C++", "Python", "JavaScript", "TypeScript", "Shell", "Rust", "HTML", "CSS", "Template", "Script"
};
static const char* const digest_kind_names[] = {
    "source", "header", "test", "doc", "build", "config", "directory"
};

typedef struct {
    char* path;
    int kind, language;
    unsigned long bytes, lines;
    unsigned char hash[BLAKE3_OUT_LEN];
    char* capture;        /* README prefix capture owned per entry (pool-safe) */
    size_t capture_len;
    int read_error;       /* DGR_* staged read result, 0 = success */
} digest_entry_t;

typedef struct {
    digest_entry_t* entries;
    size_t count, visited;
    unsigned long counts[D_CLASSES], languages[L_NONE], bytes, lines;
    unsigned long excluded_files, excluded_special, excluded_directories;
    char readme[8193];
    size_t readme_len;
    int readme_rank;
    const char* readme_path;
} digest_inventory_t;

/* Per-entry README captures must fill the inventory buffer exactly like the
 * sequential single-buffer reader did. */
#define DIGEST_README_MAX (sizeof(((digest_inventory_t*)0)->readme) - 1)
_Static_assert(DIGEST_README_MAX == (size_t)8192,
               "README capture bound must match the inventory buffer");

typedef struct {
    char* data;
    size_t len, cap;
    int failed;
} digest_buffer_t;

static int digest_error(const char* message) {
    jash_writef(jash_stderr_writer, NULL, "digest: %s\n", message);
    return 1;
}

static int digest_write_all(int fd, const char* data, size_t n);

/* Staged inventory read results. The walk thread reports staging failures
 * immediately in traversal order; worker failures are recorded per entry and
 * reported once, in the same traversal order, after the pool joins — so error
 * text and every output byte stay identical for any worker budget. */
enum { DGR_OK = 0, DGR_OPEN, DGR_BEFORE, DGR_READ, DGR_FILE_LIMIT,
       DGR_TOTAL_LIMIT, DGR_DURING, DGR_CLOSE };
static const char* const digest_read_msgs[] = {
    "inventory file staged", /* DGR_OK, never reported */
    "cannot open inventory file without following symlinks",
    "inventory file changed before read",
    "inventory file read failed",
    "file byte limit exceeded (8 MiB)",
    "total byte limit exceeded (128 MiB)",
    "inventory file changed during read",
    "inventory file close failed"
};

static const char* const digest_skipped_dirs[] = {
    ".git", ".jash", "build", "build-speed", "node_modules", ".cache", "target",
    ".venv", "venv", "__pycache__", "oci-export", ".Trash", "dist",
    "Quarentena-codes", ".DS_Store", ".idea", ".vscode", ".pytest_cache",
    ".mypy_cache", ".ruff_cache", NULL
};

static int digest_skip_dir(const char* name) {
    for (size_t i = 0; digest_skipped_dirs[i]; i++)
        if (!strcmp(name, digest_skipped_dirs[i])) return 1;
    return 0;
}

static int digest_ends(const char* name, const char* suffix) {
    size_t n = strlen(name), s = strlen(suffix);
    return n >= s && !strcmp(name + n - s, suffix);
}

static int digest_test_path(const char* path, const char* name) {
    for (const char* p = path; p < name;) {
        const char* end = strchr(p, '/');
        if (!end) break;
        size_t n = (size_t)(end - p);
        if ((n == 4 && !memcmp(p, "test", n)) ||
            (n == 5 && !memcmp(p, "tests", n)) ||
            (n == 9 && !memcmp(p, "__tests__", n))) return 1;
        p = end + 1;
    }
    const char* dot = strrchr(name, '.');
    size_t stem = dot ? (size_t)(dot - name) : strlen(name);
    return !strncmp(name, "test_", 5) ||
        (stem >= 5 && (!memcmp(name + stem - 5, "_test", 5) ||
                       !memcmp(name + stem - 5, ".test", 5) ||
                       !memcmp(name + stem - 5, ".spec", 5)));
}

static int digest_classify(const char* path, int* language) {
    const char* name = strrchr(path, '/');
    name = name ? name + 1 : path;
    const char* const builds[] = {
        "CMakeLists.txt", "CMakePresets.json", "Makefile", "makefile", "GNUmakefile",
        "meson.build", "meson_options.txt", "build.ninja", "configure", "configure.ac",
        "Dockerfile", "compose.yml", "compose.yaml", "docker-compose.yml",
        "pyproject.toml", "setup.py", "setup.cfg", "requirements.txt", "Pipfile",
        "Pipfile.lock", "poetry.lock", "uv.lock", "tox.ini", "pytest.ini",
        "package.json", "package-lock.json", "yarn.lock", "pnpm-lock.yaml",
        "bun.lock", "bun.lockb", "tsconfig.json", "jsconfig.json",
        "Cargo.toml", "Cargo.lock", "rust-toolchain", "rust-toolchain.toml", NULL
    };
    *language = L_NONE;
    for (size_t i = 0; builds[i]; i++)
        if (!strcmp(name, builds[i])) return D_BUILD;
    if (digest_ends(name, ".cmake") ||
        (!strncmp(name, "requirements-", 13) && digest_ends(name, ".txt")) ||
        (!strncmp(name, "tsconfig.", 9) && digest_ends(name, ".json"))) return D_BUILD;
    const char* dot = strrchr(name, '.');
    if (dot) {
        if (!strcmp(dot, ".c") || !strcmp(dot, ".h")) *language = L_C;
        else if (!strcmp(dot, ".cpp") || !strcmp(dot, ".cc") || !strcmp(dot, ".cxx") ||
                 !strcmp(dot, ".hpp") || !strcmp(dot, ".hh") || !strcmp(dot, ".hxx")) *language = L_CPP;
        else if (!strcmp(dot, ".py") || !strcmp(dot, ".pyi")) *language = L_PYTHON;
        else if (!strcmp(dot, ".js") || !strcmp(dot, ".jsx") ||
                 !strcmp(dot, ".mjs") || !strcmp(dot, ".cjs")) *language = L_JS;
        else if (!strcmp(dot, ".ts") || !strcmp(dot, ".tsx") ||
                 !strcmp(dot, ".mts") || !strcmp(dot, ".cts")) *language = L_TS;
        else if (!strcmp(dot, ".sh") || !strcmp(dot, ".bash") || !strcmp(dot, ".zsh")) *language = L_SHELL;
        else if (!strcmp(dot, ".rs")) *language = L_RUST;
        else if (!strcmp(dot, ".html") || !strcmp(dot, ".htm")) *language = L_HTML;
        else if (!strcmp(dot, ".css") || !strcmp(dot, ".scss") || !strcmp(dot, ".sass") || !strcmp(dot, ".less")) *language = L_CSS;
        else if (!strcmp(dot, ".vue") || !strcmp(dot, ".svelte") || !strcmp(dot, ".astro") || !strcmp(dot, ".svg")) *language = L_TEMPLATE;
        if (!strcmp(dot, ".h") || !strcmp(dot, ".hpp") || !strcmp(dot, ".hh") ||
            !strcmp(dot, ".hxx") || digest_ends(name, ".d.ts")) return D_HEADER;
    }
    if (*language != L_NONE) return digest_test_path(path, name) ? D_TEST : D_SOURCE;
    if (!strcasecmp(name, "README") || !strcasecmp(name, "LICENSE") ||
        !strcasecmp(name, "COPYING") || !strcasecmp(name, "CHANGELOG") ||
        (dot && (!strcasecmp(dot, ".md") || !strcasecmp(dot, ".rst") ||
                 !strcasecmp(dot, ".txt") || !strcasecmp(dot, ".adoc")))) return D_DOC;
    const char* const configs[] = {
        ".env", ".editorconfig", ".gitignore", ".gitattributes", ".dockerignore",
        ".npmrc", ".nvmrc", ".yarnrc", NULL
    };
    for (size_t i = 0; configs[i]; i++) if (!strcmp(name, configs[i])) return D_CONFIG;
    if (!strncmp(name, ".env.", 5)) return D_CONFIG;
    if (dot && (!strcmp(dot, ".json") || !strcmp(dot, ".toml") || !strcmp(dot, ".yaml") ||
                !strcmp(dot, ".yml") || !strcmp(dot, ".ini") || !strcmp(dot, ".cfg") ||
                !strcmp(dot, ".conf") || !strcmp(dot, ".xml") || !strcmp(dot, ".lock") ||
                !strcmp(dot, ".properties") || !strcmp(dot, ".env"))) return D_CONFIG;
    return -1;
}

static void digest_hash_hex(const unsigned char hash[BLAKE3_OUT_LEN], char out[65]) {
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < BLAKE3_OUT_LEN; i++) {
        out[2*i] = hex[hash[i] >> 4];
        out[2*i+1] = hex[hash[i] & 15];
    }
    out[64] = '\0';
}

/* Length framing is architecture-independent and cannot collide on filenames
 * containing newlines, colons, or the old file:/dir: delimiters. */
static void digest_hash_field(struct blake3* h, const void* data, size_t n) {
    unsigned char len[8];
    uint64_t v = n;
    for (int i = 0; i < 8; i++) { len[i] = (unsigned char)v; v >>= 8; }
    blake3_update(h, len, sizeof(len));
    blake3_update(h, data, n);
}

static int digest_same_stat(const struct stat* a, const struct stat* b) {
    if (a->st_dev != b->st_dev || a->st_ino != b->st_ino ||
        a->st_mode != b->st_mode || a->st_size != b->st_size) return 0;
#ifdef __APPLE__
    return a->st_mtimespec.tv_sec == b->st_mtimespec.tv_sec &&
        a->st_mtimespec.tv_nsec == b->st_mtimespec.tv_nsec &&
        a->st_ctimespec.tv_sec == b->st_ctimespec.tv_sec &&
        a->st_ctimespec.tv_nsec == b->st_ctimespec.tv_nsec;
#else
    return a->st_mtim.tv_sec == b->st_mtim.tv_sec && a->st_mtim.tv_nsec == b->st_mtim.tv_nsec &&
        a->st_ctim.tv_sec == b->st_ctim.tv_sec && a->st_ctim.tv_nsec == b->st_ctim.tv_nsec;
#endif
}

/* A configured CMake build directory can have any name. Inspect its own
 * CMakeCache.txt rather than excluding arbitrary build-* source directories.
 * A symlink marker does not qualify, and the held directory must still be the
 * inode seen during enumeration. */
static int digest_generated_cmake_dir(int parent, const char* name,
                                      const struct stat* observed) {
    int dir = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dir < 0) return -1;
    struct stat actual, marker;
    int rc = -1;
    if (fstat(dir, &actual) == 0 && digest_same_stat(observed, &actual)) {
        if (fstatat(dir, "CMakeCache.txt", &marker, AT_SYMLINK_NOFOLLOW) == 0)
            rc = S_ISREG(marker.st_mode);
        else if (errno == ENOENT) rc = 0;
    } else errno = ESTALE;
    int saved = errno;
    if (close(dir) < 0 && rc >= 0) return -1;
    if (rc < 0) errno = saved;
    return rc;
}

/* Extensionless entrypoints are observed by shebang, never executed. Binary
 * candidates cost at most 256 bytes and remain outside the content inventory. */
static int digest_shebang(int parent, const char* name, const struct stat* observed, int* language) {
    int fd = openat(parent, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return -1;
    struct stat before, after;
    char prefix[257];
    ssize_t n = -1;
    int valid = fstat(fd, &before) == 0 && S_ISREG(before.st_mode) && digest_same_stat(observed, &before);
    if (valid) {
        do { n = read(fd, prefix, sizeof(prefix) - 1); } while (n < 0 && errno == EINTR);
        valid = n >= 0 && fstat(fd, &after) == 0 && digest_same_stat(&before, &after);
    }
    if (close(fd) || !valid) return -1;
    if (n < 2 || prefix[0] != '#' || prefix[1] != '!') return 0;
    prefix[n] = '\0';
    char* newline = strchr(prefix, '\n');
    if (newline) *newline = '\0';
    *language = strstr(prefix, "python") ? L_PYTHON : strstr(prefix, "node") ? L_JS :
        strstr(prefix, "sh") ? L_SHELL : L_SCRIPT;
    return 1;
}

static int digest_readme_rank(const char* path) {
    const char* const names[] = {"README.md", "README.rst", "README.txt", "README", NULL};
    for (int i = 0; names[i]; i++) if (!strcmp(path, names[i])) return i;
    for (int i = 0; names[i]; i++) if (!strcasecmp(path, names[i])) return i + 4;
    return 100;
}

/* ── Inventory worker pool (explicit CPU budget) ──────────────────────────
 * The walk thread owns traversal, classification, entry append and staging
 * (bound check, nofollow open, identity check); workers only stream content
 * hashes for already-open fds. Entries live in a fixed pre-allocated array,
 * so worker entry pointers stay stable. The global byte budget and queue
 * state are mutex-guarded, the failure flag is atomic, and output is assembled
 * only after the pool joins, so bytes and fingerprints are identical for any
 * budget. Beyond has no pthread ABI: staged reads run inline instead. */

typedef struct {
    digest_entry_t* entry;
    int fd;
    struct stat before;
} digest_job_t;

typedef struct {
    unsigned long total_bytes;  /* global content budget; lock-guarded when pooled */
    atomic_int failed;         /* set by any worker failure; stops new dispatch */
    int worker_abort;          /* walk broke on failed; settle must report it */
    int cas_dir;               /* content store root, or -1 when disabled */
    int cas_tmp;               /* content store staging dir, or -1 */
    int cas_ok;                /* quota snapshot passed */
#ifndef JASH_TARGET_BEYOND
    digest_job_t* slots;
    size_t capacity, head, count;
    pthread_mutex_t lock;
    pthread_cond_t not_empty, not_full;
    pthread_t* threads;
    int threads_started, inflight, done, abort;
#endif
} digest_pool_t;

static int digest_total_take(digest_pool_t* pool, size_t n) {
    int over = 0;
#ifndef JASH_TARGET_BEYOND
    if (pool->capacity) pthread_mutex_lock(&pool->lock);
#endif
    if (n > DIGEST_MAX_TOTAL_BYTES - pool->total_bytes) over = 1;
    else pool->total_bytes += n;
#ifndef JASH_TARGET_BEYOND
    if (pool->capacity) pthread_mutex_unlock(&pool->lock);
#endif
    return over;
}

/* Content-addressed store (H3): when enabled, successfully read content is
 * also staged into cas/tmp/ and renamed to cas/aa/bb... on hash. Existing
 * objects are kept (same hash means same bytes); every CAS failure degrades
 * to no-store without failing the read. */
static void digest_cas_drop(digest_pool_t* pool, int tmp, const char* tmpname) {
    if (tmp >= 0) close(tmp);
    if (pool->cas_tmp >= 0 && tmpname && *tmpname)
        unlinkat(pool->cas_tmp, tmpname, 0);
}

static void digest_cas_commit(digest_pool_t* pool, int tmp, const char* tmpname,
                              const digest_entry_t* e) {
    char hex[65], target[70], shard[3];
    struct stat ost;
    digest_hash_hex(e->hash, hex);
    shard[0] = hex[0];
    shard[1] = hex[1];
    shard[2] = '\0';
    snprintf(target, sizeof(target), "%s/%s", shard, hex + 2);
    if (mkdirat(pool->cas_dir, shard, 0700) < 0 && errno != EEXIST) {
        digest_cas_drop(pool, tmp, tmpname);
        return;
    }
    if (fstatat(pool->cas_dir, target, &ost, AT_SYMLINK_NOFOLLOW) == 0) {
        if (S_ISREG(ost.st_mode) && (uint64_t)ost.st_size == (uint64_t)e->bytes) {
            /* Object already stored: same hash, same bytes. */
            digest_cas_drop(pool, tmp, tmpname);
            return;
        }
        if (ost.st_uid != geteuid()) {
            /* Foreign-owned entry in our store: hands off, keep the briefing. */
            digest_cas_drop(pool, tmp, tmpname);
            return;
        }
        /* Corrupt entry we can repair: fall through to rename. */
    } else if (errno != ENOENT) {
        digest_cas_drop(pool, tmp, tmpname);
        return;
    }
    if (renameat(pool->cas_tmp, tmpname, pool->cas_dir, target) < 0)
        digest_cas_drop(pool, tmp, tmpname);
    else close(tmp);
}

/* Streams one staged fd: content hash, byte/line accounting and optional
 * README prefix capture into the entry. Returns a DGR_* code; never emits. */
static int digest_read_job(digest_pool_t* pool, digest_entry_t* e,
                           int fd, const struct stat* before) {
    struct blake3 h;
    blake3_init(&h);
    char buf[16384];
    int last = '\n';
    int rc = DGR_OK;
    int tmp = -1;
    char tmpname[64];
    tmpname[0] = '\0';
    if (pool->cas_ok && pool->cas_dir >= 0 && pool->cas_tmp >= 0) {
        static _Atomic unsigned serial;
        snprintf(tmpname, sizeof(tmpname), ".cas-%ld-%u.tmp",
                 (long)getpid(), atomic_fetch_add(&serial, 1));
        tmp = openat(pool->cas_tmp, tmpname,
                     O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (tmp < 0) tmpname[0] = '\0';
    }
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { rc = DGR_READ; goto done; }
        if (!n) break;
        if ((size_t)n > DIGEST_MAX_FILE_BYTES - e->bytes) { rc = DGR_FILE_LIMIT; goto done; }
        if (digest_total_take(pool, (size_t)n)) { rc = DGR_TOTAL_LIMIT; goto done; }
        e->bytes += (unsigned long)n;
        blake3_update(&h, buf, (size_t)n);
        for (ssize_t i = 0; i < n; i++) if (buf[i] == '\n') e->lines++;
        last = (unsigned char)buf[n-1];
        if (e->capture) {
            size_t keep = DIGEST_README_MAX - e->capture_len;
            if (keep > (size_t)n) keep = (size_t)n;
            memcpy(e->capture + e->capture_len, buf, keep);
            e->capture_len += keep;
        }
        if (tmp >= 0 && digest_write_all(tmp, buf, (size_t)n) < 0) {
            digest_cas_drop(pool, tmp, tmpname);
            tmp = -1;
            tmpname[0] = '\0';
        }
    }
    struct stat after;
    if (fstat(fd, &after) < 0 || !digest_same_stat(before, &after) ||
        e->bytes != (uint64_t)before->st_size) { rc = DGR_DURING; goto done; }
    if (e->bytes && last != '\n') e->lines++;
    blake3_final(&h, e->hash, sizeof(e->hash));
    if (tmp >= 0) {
        digest_cas_commit(pool, tmp, tmpname, e);
        tmp = -1;
    }
done:
    if (tmp >= 0) digest_cas_drop(pool, tmp, tmpname);
    if (close(fd) < 0 && !rc) rc = DGR_CLOSE;
    return rc;
}

#ifndef JASH_TARGET_BEYOND

static void* digest_worker_main(void* arg) {
    digest_pool_t* pool = arg;
    for (;;) {
        pthread_mutex_lock(&pool->lock);
        while (!pool->count && !pool->done)
            pthread_cond_wait(&pool->not_empty, &pool->lock);
        if (!pool->count) { pthread_mutex_unlock(&pool->lock); break; }
        digest_job_t job = pool->slots[pool->head];
        pool->head = (pool->head + 1) % pool->capacity;
        pool->count--;
        pool->inflight++;
        int skip = pool->abort || atomic_load_explicit(&pool->failed, memory_order_relaxed);
        pthread_cond_signal(&pool->not_full);
        pthread_mutex_unlock(&pool->lock);
        if (skip) close(job.fd);
        else {
            int rc = digest_read_job(pool, job.entry, job.fd, &job.before);
            if (rc) {
                job.entry->read_error = rc;
                atomic_store_explicit(&pool->failed, 1, memory_order_relaxed);
                pthread_mutex_lock(&pool->lock);
                pthread_cond_broadcast(&pool->not_empty);
                pthread_cond_broadcast(&pool->not_full);
                pthread_mutex_unlock(&pool->lock);
            }
        }
        pthread_mutex_lock(&pool->lock);
        pool->inflight--;
        if (!pool->inflight && !pool->count)
            pthread_cond_broadcast(&pool->not_empty);
        pthread_mutex_unlock(&pool->lock);
    }
    return NULL;
}

/* Bounded queue: capacity slots bound open file descriptors (capacity +
 * workers). Any allocation or thread spawn failure degrades to inline reads;
 * the digest never fails because the budget could not be met. */
static void digest_pool_init(digest_pool_t* pool, int workers) {
    memset(pool, 0, sizeof(*pool));
    atomic_init(&pool->failed, 0);
    /* fd 0 is valid: disabled CAS fds must be -1, never zero from memset. */
    pool->cas_dir = -1;
    pool->cas_tmp = -1;
    if (workers <= 1) return;
    if (pthread_mutex_init(&pool->lock, NULL) != 0) return;
    if (pthread_cond_init(&pool->not_empty, NULL) != 0) {
        pthread_mutex_destroy(&pool->lock);
        return;
    }
    if (pthread_cond_init(&pool->not_full, NULL) != 0) {
        pthread_cond_destroy(&pool->not_empty);
        pthread_mutex_destroy(&pool->lock);
        return;
    }
    size_t capacity = (size_t)workers * 2;
    if (capacity < 8) capacity = 8;
    pool->slots = calloc(capacity, sizeof(*pool->slots));
    pool->threads = calloc((size_t)workers, sizeof(*pool->threads));
    if (!pool->slots || !pool->threads) {
        free(pool->slots);
        free(pool->threads);
        pool->slots = NULL;
        pool->threads = NULL;
        pthread_cond_destroy(&pool->not_full);
        pthread_cond_destroy(&pool->not_empty);
        pthread_mutex_destroy(&pool->lock);
        return;
    }
    pool->capacity = capacity;
    for (int i = 0; i < workers; i++) {
        if (pthread_create(&pool->threads[i], NULL, digest_worker_main, pool) != 0) break;
        pool->threads_started++;
    }
    if (!pool->threads_started) {
        free(pool->slots);
        free(pool->threads);
        pool->slots = NULL;
        pool->threads = NULL;
        pool->capacity = 0;
        pthread_cond_destroy(&pool->not_full);
        pthread_cond_destroy(&pool->not_empty);
        pthread_mutex_destroy(&pool->lock);
    }
}

#else /* JASH_TARGET_BEYOND: inline staging only, no queue or threads */

static void digest_pool_init(digest_pool_t* pool, int workers) {
    memset(pool, 0, sizeof(*pool));
    atomic_init(&pool->failed, 0);
    pool->cas_dir = -1;
    pool->cas_tmp = -1;
    (void)workers;
}

#endif

/* Returns 0 after handing fd to a worker (or the caller on Beyond/inline);
 * inline failures return their DGR_* code so staging can emit immediately. */
static int digest_pool_submit(digest_pool_t* pool, digest_entry_t* e,
                              int fd, const struct stat* before) {
#ifndef JASH_TARGET_BEYOND
    if (pool->capacity) {
        pthread_mutex_lock(&pool->lock);
        while (pool->count == pool->capacity &&
               !atomic_load_explicit(&pool->failed, memory_order_relaxed))
            pthread_cond_wait(&pool->not_full, &pool->lock);
        if (atomic_load_explicit(&pool->failed, memory_order_relaxed)) {
            pthread_mutex_unlock(&pool->lock);
            close(fd);
            return 0;
        }
        digest_job_t* slot = &pool->slots[(pool->head + pool->count) % pool->capacity];
        slot->entry = e;
        slot->fd = fd;
        slot->before = *before;
        pool->count++;
        pthread_cond_signal(&pool->not_empty);
        pthread_mutex_unlock(&pool->lock);
        return 0;
    }
#endif
    int rc = digest_read_job(pool, e, fd, before);
    if (rc) e->read_error = rc;
    return rc;
}

/* Joins the pool before any entry is scanned or aggregated. Normal completion
 * waits for queued and in-flight reads; an aborted walk (walk_failed) lets
 * workers drop queued fds instead. Beyond: no-op. */
static void digest_pool_finish(digest_pool_t* pool, int walk_failed) {
#ifndef JASH_TARGET_BEYOND
    if (pool->capacity) {
        pthread_mutex_lock(&pool->lock);
        pool->done = 1;
        pool->abort = walk_failed;
        pthread_cond_broadcast(&pool->not_empty);
        if (!walk_failed)
            while (pool->count || pool->inflight)
                pthread_cond_wait(&pool->not_empty, &pool->lock);
        pthread_mutex_unlock(&pool->lock);
        for (int i = 0; i < pool->threads_started; i++)
            pthread_join(pool->threads[i], NULL);
        free(pool->threads);
        free(pool->slots);
        pool->threads = NULL;
        pool->slots = NULL;
        pthread_cond_destroy(&pool->not_full);
        pthread_cond_destroy(&pool->not_empty);
        pthread_mutex_destroy(&pool->lock);
        pool->capacity = 0;
        pool->threads_started = 0;
    }
#else
    (void)pool;
    (void)walk_failed;
#endif
}

/* Walk-thread staging: bound check, nofollow open and identity check emit
 * immediately in traversal order, exactly like the sequential reader. The
 * capture decision mirrors the sequential running-best rule evaluated in the
 * same traversal order. */
static int digest_stage_file(int parent, const char* name, const struct stat* observed,
                             digest_entry_t* e, digest_inventory_t* inv,
                             digest_pool_t* pool) {
    if (observed->st_size < 0 || (uint64_t)observed->st_size > DIGEST_MAX_FILE_BYTES) {
        digest_error(digest_read_msgs[DGR_FILE_LIMIT]);
        return 1;
    }
    int fd = openat(parent, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) { digest_error(digest_read_msgs[DGR_OPEN]); return 1; }
    struct stat before;
    if (fstat(fd, &before) < 0 || !S_ISREG(before.st_mode) || !digest_same_stat(observed, &before)) {
        close(fd);
        digest_error(digest_read_msgs[DGR_BEFORE]);
        return 1;
    }
    int rank = digest_readme_rank(e->path);
    int capture = rank < inv->readme_rank ||
        (rank < 100 && rank == inv->readme_rank && strcmp(e->path, inv->readme_path) < 0);
    if (capture) {
        e->capture = malloc(DIGEST_README_MAX);
        if (!e->capture) { close(fd); digest_error("inventory allocation failed"); return 1; }
        inv->readme_rank = rank;
        inv->readme_path = e->path;
    }
    int rc = digest_pool_submit(pool, e, fd, &before);
    if (rc) { digest_error(digest_read_msgs[rc]); return 1; }
    return 0;
}

/* Consumes fd. All descendant access is relative to held directory descriptors,
 * with nofollow and stat/open identity checks; FIFOs are never read. */
static int digest_walk(int fd, const char* rel, unsigned depth,
                       digest_inventory_t* inv, digest_pool_t* pool) {
    struct stat before, after;
    if (fstat(fd, &before) < 0) { close(fd); return digest_error("cannot stat directory"); }
    DIR* dir = fdopendir(fd);
    if (!dir) { close(fd); return digest_error("cannot read directory"); }
    int rc = 0;
    for (;;) {
        if (atomic_load_explicit(&pool->failed, memory_order_relaxed)) {
            pool->worker_abort = 1;
            rc = 1;
            break;
        }
        errno = 0;
        struct dirent* de = readdir(dir);
        if (!de) { if (errno) rc = digest_error("directory read failed"); break; }
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (++inv->visited > DIGEST_MAX_ENTRIES) {
            rc = digest_error("entry limit exceeded (32768 across the tree)"); break;
        }
        struct stat st;
        if (fstatat(fd, de->d_name, &st, AT_SYMLINK_NOFOLLOW) < 0) {
            rc = digest_error("cannot stat inventory entry"); break;
        }
        int isdir = S_ISDIR(st.st_mode);
        if (isdir && digest_skip_dir(de->d_name)) { inv->excluded_directories++; continue; }
        if (isdir) {
            int generated = digest_generated_cmake_dir(fd, de->d_name, &st);
            if (generated < 0) {
                rc = digest_error("cannot inspect inventory directory"); break;
            }
            if (generated) { inv->excluded_directories++; continue; }
        }
        if (!isdir && !S_ISREG(st.st_mode)) { inv->excluded_special++; continue; }
        char path[DIGEST_MAX_PATH];
        int n = snprintf(path, sizeof(path), "%s%s%s", rel, *rel ? "/" : "", de->d_name);
        if (n < 0 || (size_t)n >= sizeof(path)) {
            rc = digest_error("relative path limit exceeded (4095 bytes)"); break;
        }
        int language = L_NONE;
        int kind = isdir ? D_DIR : digest_classify(path, &language);
        if (kind < 0 && !strchr(de->d_name, '.')) {
            int script = digest_shebang(fd, de->d_name, &st, &language);
            if (script < 0) { rc = digest_error("cannot inspect entrypoint shebang"); break; }
            const char* basename = strrchr(path, '/');
            basename = basename ? basename + 1 : path;
            if (script) kind = digest_test_path(path, basename) ? D_TEST : D_SOURCE;
        }
        if (kind < 0) { inv->excluded_files++; continue; }
        digest_entry_t* e = &inv->entries[inv->count++];
        e->path = strdup(path);
        if (!e->path) { rc = digest_error("inventory allocation failed"); break; }
        e->language = language;
        e->kind = kind;
        inv->counts[e->kind]++;
        if (e->language != L_NONE) inv->languages[e->language]++;
        if (isdir) {
            if (depth >= DIGEST_MAX_DEPTH) {
                rc = digest_error("directory depth limit exceeded (32)"); break;
            }
            int child = openat(fd, de->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (child < 0) { rc = digest_error("cannot open inventory directory"); break; }
            struct stat opened;
            if (fstat(child, &opened) < 0 || !digest_same_stat(&st, &opened)) {
                close(child); rc = digest_error("inventory directory changed before scan"); break;
            }
            rc = digest_walk(child, path, depth + 1, inv, pool);
        } else rc = digest_stage_file(fd, de->d_name, &st, e, inv, pool);
        if (rc) break;
    }
    if (!rc && (fstat(fd, &after) < 0 || !digest_same_stat(&before, &after)))
        rc = digest_error("inventory directory changed during scan");
    if (closedir(dir) < 0 && !rc) rc = digest_error("directory close failed");
    return rc;
}

/* Post-join, single-threaded: report the first staged read failure in traversal
 * order (the exact message the sequential reader would have emitted), then
 * byte/line sums and the README winner — byte-identical to sequential output. */
static int digest_inventory_settle(digest_inventory_t* inv) {
    for (size_t i = 0; i < inv->count; i++)
        if (inv->entries[i].read_error)
            return digest_error(digest_read_msgs[inv->entries[i].read_error]);
    for (size_t i = 0; i < inv->count; i++) {
        inv->bytes += inv->entries[i].bytes;
        inv->lines += inv->entries[i].lines;
    }
    digest_entry_t* best = NULL;
    int best_rank = 100;
    for (size_t i = 0; i < inv->count; i++) {
        digest_entry_t* e = &inv->entries[i];
        if (!e->capture) continue;
        int rank = digest_readme_rank(e->path);
        if (!best || rank < best_rank ||
            (rank == best_rank && strcmp(e->path, best->path) < 0)) {
            best = e;
            best_rank = rank;
        }
    }
    if (best) {
        memcpy(inv->readme, best->capture, best->capture_len);
        inv->readme_len = best->capture_len;
        inv->readme[inv->readme_len] = '\0';
        inv->readme_rank = best_rank;
        inv->readme_path = best->path;
    }
    return 0;
}

static int digest_entry_cmp(const void* a, const void* b) {
    return strcmp(((const digest_entry_t*)a)->path, ((const digest_entry_t*)b)->path);
}

static void digest_fingerprint(digest_inventory_t* inv, const char* root, char out[65]) {
    qsort(inv->entries, inv->count, sizeof(*inv->entries), digest_entry_cmp);
    struct blake3 h;
    blake3_init(&h);
    digest_hash_field(&h, DIGEST_GENERATOR_VERSION, strlen(DIGEST_GENERATOR_VERSION));
    digest_hash_field(&h, root, strlen(root));
    for (size_t i = 0; i < inv->count; i++) {
        digest_entry_t* e = &inv->entries[i];
        digest_hash_field(&h, e->kind == D_DIR ? "dir" : "file", e->kind == D_DIR ? 3 : 4);
        digest_hash_field(&h, e->path, strlen(e->path));
        if (e->kind != D_DIR) digest_hash_field(&h, e->hash, sizeof(e->hash));
    }
    unsigned char hash[BLAKE3_OUT_LEN];
    blake3_final(&h, hash, sizeof(hash));
    digest_hash_hex(hash, out);
}

static long digest_buffer_writer(void* ctx, const char* data, size_t n) {
    digest_buffer_t* b = ctx;
    if (b->failed) return -1;
    if (n > DIGEST_MAX_OUTPUT_BYTES - b->len) { b->failed = 1; return -1; }
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 4096;
        if (cap < b->len + n + 1) cap = b->len + n + 1;
        char* next = realloc(b->data, cap);
        if (!next) { b->failed = 1; return -1; }
        b->data = next;
        b->cap = cap;
    }
    memcpy(b->data + b->len, data, n);
    b->len += n;
    b->data[b->len] = '\0';
    return (long)n;
}

static size_t digest_utf8_span(const unsigned char* p, size_t n) {
    unsigned c = p[0], value;
    size_t len;
    if (c >= 0xc2 && c <= 0xdf) { len = 2; value = c & 0x1f; }
    else if (c >= 0xe0 && c <= 0xef) { len = 3; value = c & 0x0f; }
    else if (c >= 0xf0 && c <= 0xf4) { len = 4; value = c & 7; }
    else return 0;
    if (len > n) return 0;
    for (size_t i = 1; i < len; i++) {
        if ((p[i] & 0xc0) != 0x80) return 0;
        value = (value << 6) | (p[i] & 0x3f);
    }
    if (value < 0xa0 || (len == 3 && value < 0x800) || (len == 4 && value < 0x10000) ||
        (value >= 0xd800 && value <= 0xdfff) || value > 0x10ffff) return 0;
    return len;
}

/* Preserve valid UTF-8; visibly escape malformed bytes and control bytes.
 * README is explicitly attributed, quoted data, never treated as commands. */
static void digest_text(digest_buffer_t* b, const char* text, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)text[i];
        size_t span = c >= 128 ? digest_utf8_span((const unsigned char*)text + i, n - i) : 0;
        if (span) {
            digest_buffer_writer(b, text + i, span);
            i += span - 1;
        } else if (c < 32 || c >= 127 || c == '`' || c == '\\') {
            char escaped[5];
            snprintf(escaped, sizeof(escaped), "\\x%02x", c);
            digest_buffer_writer(b, escaped, 4);
        } else digest_buffer_writer(b, text + i, 1);
    }
}

static const char* digest_level_name(digest_level_t level) {
    return level == DIGEST_BRIEF ? "brief" : level == DIGEST_DEEP ? "deep" : "standard";
}

static void digest_list(digest_buffer_t* b, const digest_inventory_t* inv,
                         int kind, size_t limit, const char* title) {
    size_t total = kind < 0 ? inv->count : inv->counts[kind];
    size_t shown = total < limit ? total : limit;
    jash_writef(digest_buffer_writer, b, "\n## %s (showing %lu of %lu)\n\n",
                title, (unsigned long)shown, (unsigned long)total);
    for (size_t i = 0, count = 0; i < inv->count && count < limit; i++) {
        const digest_entry_t* e = &inv->entries[i];
        if (kind >= 0 && e->kind != kind) continue;
        count++;
        jash_writes(digest_buffer_writer, b, "- `");
        digest_text(b, e->path, strlen(e->path));
        if (e->kind == D_DIR) jash_writes(digest_buffer_writer, b, "/");
        jash_writes(digest_buffer_writer, b, "`");
        if (kind < 0 && e->kind != D_DIR)
            jash_writef(digest_buffer_writer, b, " (%lu bytes, %lu lines)", e->bytes, e->lines);
        jash_writes(digest_buffer_writer, b, "\n");
    }
    if (!total) jash_writes(digest_buffer_writer, b, "(none observed)\n");
}

static int digest_compose(const digest_cfg_t* cfg, const char* root,
                           const digest_inventory_t* inv, const char* fingerprint,
                           digest_buffer_t* b) {
    const char* name = strrchr(root, '/');
    name = name && name[1] ? name + 1 : root;
    jash_writes(digest_buffer_writer, b, "# Project Digest\n\n**project:** ");
    digest_text(b, name, strlen(name));
    jash_writes(digest_buffer_writer, b, "  \n**root:** `");
    digest_text(b, root, strlen(root));
    jash_writef(digest_buffer_writer, b,
        "`  \n**generator:** %s  \n**tree fingerprint:** `%s`  \n**level:** %s  \n"
        "**source files:** %lu | **headers:** %lu | **tests:** %lu | **docs:** %lu | "
        "**build files:** %lu | **configuration files:** %lu  \n"
        "**directories:** %lu | **total lines:** %lu | **total bytes:** %lu  \n\n",
        DIGEST_GENERATOR_VERSION, fingerprint, digest_level_name(cfg->level),
        inv->counts[D_SOURCE], inv->counts[D_HEADER], inv->counts[D_TEST], inv->counts[D_DOC],
        inv->counts[D_BUILD], inv->counts[D_CONFIG], inv->counts[D_DIR], inv->lines, inv->bytes);
    jash_writes(digest_buffer_writer, b, "**Observed code languages** (includes headers/tests): ");
    int have = 0;
    for (int i = 0; i < L_NONE; i++) if (inv->languages[i]) {
        jash_writef(digest_buffer_writer, b, "%s%s (%lu)", have ? ", " : "", digest_languages[i], inv->languages[i]);
        have = 1;
    }
    if (!have) jash_writes(digest_buffer_writer, b, "none recognized");
    jash_writes(digest_buffer_writer, b,
        "\n\nCounts are exclusive filename/path classifications; only recognized source/doc/config files contribute bytes and lines.\n");
    if (inv->readme_path) {
        size_t max_bytes = cfg->level == DIGEST_BRIEF ? 1024 : cfg->level == DIGEST_STANDARD ? 4096 : 8192;
        unsigned max_lines = cfg->level == DIGEST_BRIEF ? 12 : cfg->level == DIGEST_STANDARD ? 40 : 80;
        jash_writes(digest_buffer_writer, b, "\n## README excerpt — `");
        digest_text(b, inv->readme_path, strlen(inv->readme_path));
        jash_writef(digest_buffer_writer, b, "` (up to %lu bytes / %u lines)\n\n> ",
                    (unsigned long)max_bytes, max_lines);
        size_t n = inv->readme_len < max_bytes ? inv->readme_len : max_bytes;
        unsigned lines = 0;
        for (size_t i = 0; i < n && lines < max_lines;) {
            size_t end = i;
            while (end < n && inv->readme[end] != '\n') end++;
            digest_text(b, inv->readme + i, end - i);
            lines++;
            jash_writes(digest_buffer_writer, b, "\n> ");
            i = end + 1;
        }
        jash_writes(digest_buffer_writer, b, "\n");
    } else jash_writes(digest_buffer_writer, b, "\nNo root README observed; identity is the directory name.\n");
    if (cfg->level == DIGEST_DEEP) digest_list(b, inv, -1, DIGEST_MAX_ENTRIES, "Observed inventory");
    else {
        size_t limit = cfg->level == DIGEST_BRIEF ? 12 : 40;
        digest_list(b, inv, D_BUILD, limit, "Build/configuration files");
        if (cfg->level == DIGEST_STANDARD) {
            digest_list(b, inv, D_SOURCE, limit, "Source files");
            digest_list(b, inv, D_HEADER, limit, "Header files");
            digest_list(b, inv, D_TEST, limit, "Test files");
            digest_list(b, inv, D_DOC, limit, "Documentation files");
            digest_list(b, inv, D_CONFIG, limit, "Configuration files");
        }
    }
    jash_writes(digest_buffer_writer, b,
        "\n---\nObserved files only; no architecture, risk, recent-work or build-command inference.\n"
        "Opaque/unrecognized files, symlinks/special files and ignored directories (fixed names or CMakeCache marker) are excluded; .gitignore is not evaluated.\n"
        "Bounds: depth 32, entries 32768, file 8 MiB, total 128 MiB, rendered output 8 MiB.\n");
    return b->failed ? digest_error("render allocation or output byte limit exceeded (8 MiB)") : 0;
}

/* Renders one manifest-shaped record; with oldhex != NULL the record gains
 * an "old_blake3" trailer (modified entries). */
static void digest_diff_record_trailer(jash_json_writer_t* json,
                                       const digest_entry_t* e, const char* oldhex) {
    jash_jsonw_raw(json, "{\"path\":");
    jash_jsonw_string_or_bytes(json, e->path);
    jash_jsonw_raw(json, ",\"kind\":");
    jash_jsonw_string(json, digest_kind_names[e->kind]);
    jash_jsonw_raw(json, ",\"language\":");
    if (e->language == L_NONE) jash_jsonw_raw(json, "null");
    else jash_jsonw_string(json, digest_languages[e->language]);
    if (e->kind != D_DIR) {
        char hash[65];
        digest_hash_hex(e->hash, hash);
        jash_jsonw_raw(json, ",\"blake3\":");
        jash_jsonw_string(json, hash);
        jash_jsonw_raw(json, ",\"bytes\":");
        jash_jsonw_u64(json, e->bytes);
        jash_jsonw_raw(json, ",\"lines\":");
        jash_jsonw_u64(json, e->lines);
    }
    if (oldhex) {
        jash_jsonw_raw(json, ",\"old_blake3\":");
        jash_jsonw_string(json, oldhex);
    }
    jash_jsonw_raw(json, "}");
}

static void digest_diff_record(jash_json_writer_t* json, const digest_entry_t* e) {
    digest_diff_record_trailer(json, e, NULL);
}

static int digest_manifest(const char* root, const digest_inventory_t* inv,
                           const char* fingerprint, digest_buffer_t* b) {
    jash_json_writer_t json;
    jash_jsonw_init(&json, digest_buffer_writer, b);
    jash_jsonw_raw(&json, "{\"schema\":\"jash.project-observation.v1\",\"generator\":");
    jash_jsonw_string(&json, DIGEST_GENERATOR_VERSION);
    jash_jsonw_raw(&json, ",\"root\":"); jash_jsonw_string_or_bytes(&json, root);
    jash_jsonw_raw(&json, ",\"fingerprint\":"); jash_jsonw_string(&json, fingerprint);
    jash_jsonw_raw(&json, ",\"consistency\":\"best-effort\",\"coverage\":{\"gitignore_evaluated\":false,\"ignored_directories\":[");
    for (size_t i = 0; digest_skipped_dirs[i]; i++) {
        if (i) jash_jsonw_raw(&json, ",");
        jash_jsonw_string(&json, digest_skipped_dirs[i]);
    }
    jash_jsonw_raw(&json, "],\"excluded_files\":"); jash_jsonw_u64(&json, inv->excluded_files);
    jash_jsonw_raw(&json, ",\"excluded_special\":"); jash_jsonw_u64(&json, inv->excluded_special);
    jash_jsonw_raw(&json, ",\"excluded_directories\":"); jash_jsonw_u64(&json, inv->excluded_directories);
    jash_jsonw_raw(&json, ",\"scope\":\"recognized source, docs, build, config and directories; opaque files excluded\"},\"entries\":[");
    for (size_t i = 0; i < inv->count; i++) {
        if (i) jash_jsonw_raw(&json, ",");
        digest_diff_record(&json, &inv->entries[i]);
    }
    jash_jsonw_raw(&json, "]}\n");
    return jash_jsonw_finish(&json) || b->failed ? digest_error("manifest encoding or output limit failed") : 0;
}

/* ── Diff mode: strict prior-manifest reader ────────────────────────────
 * A base file is harness input, so schema validation is the defense: exact
 * schema tag, exact generator version, required fields with exact types,
 * lowercase-hex hashes, uint64 counters, no duplicate paths. Unknown
 * top-level/coverage fields are skipped (forward-compatible reader); every
 * entry field is strict because the generator gate already pins the shape.
 * Paths accept both forms the manifest writer emits: JSON strings (valid
 * UTF-8) or {"encoding":"base64","data":"..."} objects. */

#define DIGEST_DIFF_MAX_BASE_BYTES (32u * 1024u * 1024u)

typedef struct {
    const unsigned char* p;
    const unsigned char* end;
} digest_json_t;

typedef struct {
    digest_entry_t* entries;
    size_t count, cap;
    char base_fp[65];
} digest_base_t;

static int djs_ws(digest_json_t* j) {
    while (j->p < j->end && (*j->p == ' ' || *j->p == '\t' ||
                             *j->p == '\n' || *j->p == '\r')) j->p++;
    return j->p < j->end;
}

static int djs_expect(digest_json_t* j, char c) {
    if (!djs_ws(j) || *j->p != (unsigned char)c) return -1;
    j->p++;
    return 0;
}

static int djs_lit(digest_json_t* j, const char* word) {
    size_t n = strlen(word);
    if ((size_t)(j->end - j->p) < n || memcmp(j->p, word, n)) return -1;
    j->p += n;
    return 0;
}

static int djs_push_utf8(char** buf, size_t* len, size_t* cap, unsigned value) {
    unsigned char enc[4];
    size_t n;
    if (value < 0x80) { enc[0] = (unsigned char)value; n = 1; }
    else if (value < 0x800) {
        enc[0] = (unsigned char)(0xc0 | (value >> 6));
        enc[1] = (unsigned char)(0x80 | (value & 0x3f));
        n = 2;
    } else if (value < 0x10000) {
        enc[0] = (unsigned char)(0xe0 | (value >> 12));
        enc[1] = (unsigned char)(0x80 | ((value >> 6) & 0x3f));
        enc[2] = (unsigned char)(0x80 | (value & 0x3f));
        n = 3;
    } else {
        enc[0] = (unsigned char)(0xf0 | (value >> 18));
        enc[1] = (unsigned char)(0x80 | ((value >> 12) & 0x3f));
        enc[2] = (unsigned char)(0x80 | ((value >> 6) & 0x3f));
        enc[3] = (unsigned char)(0x80 | (value & 0x3f));
        n = 4;
    }
    if (*len + n + 1 > *cap) {
        size_t ncap = *cap ? *cap * 2 : 64;
        if (ncap < *len + n + 1) ncap = *len + n + 1;
        char* next = realloc(*buf, ncap);
        if (!next) return -1;
        *buf = next;
        *cap = ncap;
    }
    memcpy(*buf + *len, enc, n);
    *len += n;
    return 0;
}

static int digest_utf8_valid(const char* s, size_t n) {
    size_t i = 0;
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        size_t span;
        unsigned value;
        if (c < 0x80) { i++; continue; }
        else if (c >= 0xc2 && c <= 0xdf) { span = 2; value = c & 0x1f; }
        else if (c >= 0xe0 && c <= 0xef) { span = 3; value = c & 0x0f; }
        else if (c >= 0xf0 && c <= 0xf4) { span = 4; value = c & 7; }
        else return 0;
        if (span > n - i) return 0;
        for (size_t k = 1; k < span; k++) {
            if (((unsigned char)s[i+k] & 0xc0) != 0x80) return 0;
            value = (value << 6) | ((unsigned char)s[i+k] & 0x3f);
        }
        if (value < 0x80 || (span == 3 && value < 0x800) ||
            (span == 4 && value < 0x10000) ||
            (value >= 0xd800 && value <= 0xdfff) || value > 0x10ffff) return 0;
        i += span;
    }
    return 1;
}

static int djs_hex4(digest_json_t* j, unsigned* out) {
    if ((size_t)(j->end - j->p) < 4) return -1;
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        unsigned char c = j->p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else return -1;
    }
    j->p += 4;
    *out = v;
    return 0;
}

/* Parses a JSON string into a NUL-terminated, UTF-8-validated buffer.
 * Embedded NUL (raw or \u0000) is rejected: paths are C strings. */
static int djs_string(digest_json_t* j, char** out, size_t* nout) {
    if (djs_expect(j, '"')) return -1;
    char* buf = NULL;
    size_t len = 0, cap = 0;
    int rc = -1;
    for (;;) {
        if (j->p >= j->end) goto done;
        unsigned char c = *j->p++;
        if (c == '"') { rc = 0; goto done; }
        if (c == '\\') {
            if (j->p >= j->end) goto done;
            unsigned char e = *j->p++;
            if (e == 'u') {
                unsigned value, low;
                if (djs_hex4(j, &value)) goto done;
                if (value >= 0xd800 && value <= 0xdbff) {
                    if (djs_lit(j, "\\u") || djs_hex4(j, &low) ||
                        low < 0xdc00 || low > 0xdfff) goto done;
                    value = 0x10000 + ((value - 0xd800) << 10) + (low - 0xdc00);
                } else if (value >= 0xdc00 && value <= 0xdfff) goto done;
                if (value == 0 || value > 0x10ffff) goto done;
                if (djs_push_utf8(&buf, &len, &cap, value)) goto done;
            } else {
                unsigned value;
                switch (e) {
                case '"': value = '"'; break;
                case '\\': value = '\\'; break;
                case '/': value = '/'; break;
                case 'b': value = '\b'; break;
                case 'f': value = '\f'; break;
                case 'n': value = '\n'; break;
                case 'r': value = '\r'; break;
                case 't': value = '\t'; break;
                default: goto done;
                }
                if (djs_push_utf8(&buf, &len, &cap, value)) goto done;
            }
        } else if (c < 0x20) goto done;
        else {
            if (len + 2 > cap) {
                size_t ncap = cap ? cap * 2 : 64;
                if (ncap < len + 2) ncap = len + 2;
                char* next = realloc(buf, ncap);
                if (!next) goto done;
                buf = next;
                cap = ncap;
            }
            buf[len++] = (char)c;
        }
    }
done:
    if (!rc) {
        if (!digest_utf8_valid(buf ? buf : "", len)) rc = -1;
        else {
            if (!buf) {
                buf = malloc(1);
                if (!buf) rc = -1;
            }
            if (!rc) {
                buf[len] = '\0';
                *out = buf;
                *nout = len;
                return 0;
            }
        }
    }
    free(buf);
    return rc;
}

static int djs_uint(digest_json_t* j, uint64_t* out) {
    if (!djs_ws(j)) return -1;
    uint64_t v = 0;
    int digits = 0;
    while (j->p < j->end && *j->p >= '0' && *j->p <= '9') {
        unsigned d = (unsigned)(*j->p++ - '0');
        if (v > (UINT64_MAX - d) / 10) return -1;
        v = v * 10 + d;
        digits++;
    }
    if (!digits) return -1;
    *out = v;
    return 0;
}

/* Strict number skip (also used for ignored fields: corruption there still
 * fails the base, it just cannot affect the diff). */
static int djs_number(digest_json_t* j) {
    if (!djs_ws(j)) return -1;
    if (*j->p == '-') {
        if (++j->p >= j->end) return -1;
    }
    if (j->p >= j->end) return -1;
    if (*j->p == '0') j->p++;
    else if (*j->p >= '1' && *j->p <= '9') {
        do j->p++; while (j->p < j->end && *j->p >= '0' && *j->p <= '9');
    } else return -1;
    if (j->p < j->end && *j->p == '.') {
        if (++j->p >= j->end || *j->p < '0' || *j->p > '9') return -1;
        do j->p++; while (j->p < j->end && *j->p >= '0' && *j->p <= '9');
    }
    if (j->p < j->end && (*j->p == 'e' || *j->p == 'E')) {
        if (++j->p >= j->end) return -1;
        if (*j->p == '+' || *j->p == '-') {
            if (++j->p >= j->end) return -1;
        }
        if (*j->p < '0' || *j->p > '9') return -1;
        do j->p++; while (j->p < j->end && *j->p >= '0' && *j->p <= '9');
    }
    return 0;
}

static int djs_skip(digest_json_t* j) {
    if (!djs_ws(j)) return -1;
    unsigned char c = *j->p;
    if (c == '"') {
        char* s = NULL;
        size_t n = 0;
        int rc = djs_string(j, &s, &n);
        free(s);
        return rc;
    }
    if (c == '{') {
        j->p++;
        if (djs_ws(j) && *j->p == '}') { j->p++; return 0; }
        for (;;) {
            char* k = NULL;
            size_t kn = 0;
            if (djs_string(j, &k, &kn)) return -1;
            free(k);
            if (djs_expect(j, ':') || djs_skip(j)) return -1;
            if (!djs_ws(j)) return -1;
            if (*j->p == ',') { j->p++; continue; }
            if (*j->p == '}') { j->p++; return 0; }
            return -1;
        }
    }
    if (c == '[') {
        j->p++;
        if (djs_ws(j) && *j->p == ']') { j->p++; return 0; }
        for (;;) {
            if (djs_skip(j)) return -1;
            if (!djs_ws(j)) return -1;
            if (*j->p == ',') { j->p++; continue; }
            if (*j->p == ']') { j->p++; return 0; }
            return -1;
        }
    }
    if (c == 't') return djs_lit(j, "true");
    if (c == 'f') return djs_lit(j, "false");
    if (c == 'n') return djs_lit(j, "null");
    return djs_number(j);
}

static int digest_b64val(unsigned char c, unsigned* v) {
    if (c >= 'A' && c <= 'Z') *v = (unsigned)(c - 'A');
    else if (c >= 'a' && c <= 'z') *v = (unsigned)(c - 'a' + 26);
    else if (c >= '0' && c <= '9') *v = (unsigned)(c - '0' + 52);
    else if (c == '+') *v = 62;
    else if (c == '/') *v = 63;
    else return -1;
    return 0;
}

static int digest_b64dec(const char* s, size_t n, unsigned char** out, size_t* m) {
    if (!n || n % 4) return -1;
    size_t pad = 0;
    if (s[n-1] == '=') {
        pad++;
        if (n > 1 && s[n-2] == '=') pad++;
    }
    for (size_t i = 0; i < n - pad; i++)
        if (s[i] == '=') return -1;
    size_t len = n / 4 * 3 - pad;
    if (!len || len > DIGEST_MAX_PATH - 1) return -1;
    unsigned char* buf = malloc(len + 1);
    if (!buf) return -1;
    for (size_t i = 0; i < n; i += 4) {
        unsigned v[4];
        for (int k = 0; k < 4; k++) {
            if (s[i+k] == '=') v[k] = 0;
            else if (digest_b64val((unsigned char)s[i+k], &v[k])) {
                free(buf);
                return -1;
            }
        }
        buf[i/4*3] = (unsigned char)((v[0] << 2) | (v[1] >> 4));
        if (i/4*3 + 1 < len) buf[i/4*3+1] = (unsigned char)((v[1] << 4) | (v[2] >> 2));
        if (i/4*3 + 2 < len) buf[i/4*3+2] = (unsigned char)((v[2] << 6) | v[3]);
    }
    if (memchr(buf, '\0', len)) { free(buf); return -1; }
    buf[len] = '\0';
    *out = buf;
    *m = len;
    return 0;
}

/* Path in either writer form: JSON string, or the exact base64 object shape. */
static int djs_path(digest_json_t* j, char** out, size_t* nout) {
    if (!djs_ws(j)) return -1;
    if (*j->p == '"') return djs_string(j, out, nout);
    if (*j->p != '{') return -1;
    j->p++;
    char *k1 = NULL, *v1 = NULL, *k2 = NULL, *v2 = NULL;
    size_t n1 = 0, m1 = 0, n2 = 0, m2 = 0;
    int rc = -1;
    if (djs_string(j, &k1, &n1) || djs_expect(j, ':') || djs_string(j, &v1, &m1) ||
        djs_expect(j, ',') || djs_string(j, &k2, &n2) || djs_expect(j, ':') ||
        djs_string(j, &v2, &m2) || djs_expect(j, '}')) goto done;
    if (strcmp(k1, "encoding") || strcmp(v1, "base64") || strcmp(k2, "data")) goto done;
    unsigned char* raw = NULL;
    size_t rawlen = 0;
    if (digest_b64dec(v2, m2, &raw, &rawlen)) goto done;
    *out = (char*)raw;
    *nout = rawlen;
    rc = 0;
done:
    free(k1);
    free(v1);
    free(k2);
    free(v2);
    return rc;
}

static int digest_hex_hash(digest_json_t* j, unsigned char out[BLAKE3_OUT_LEN]) {
    if (!djs_ws(j) || (size_t)(j->end - j->p) < 66 || j->p[0] != '"' ||
        j->p[65] != '"') return -1;
    for (int i = 0; i < 32; i++) {
        unsigned hi, lo;
        unsigned char a = j->p[1+2*i], b = j->p[1+2*i+1];
        if (a >= '0' && a <= '9') hi = (unsigned)(a - '0');
        else if (a >= 'a' && a <= 'f') hi = (unsigned)(a - 'a' + 10);
        else return -1;
        if (b >= '0' && b <= '9') lo = (unsigned)(b - '0');
        else if (b >= 'a' && b <= 'f') lo = (unsigned)(b - 'a' + 10);
        else return -1;
        out[i] = (unsigned char)((hi << 4) | lo);
    }
    j->p += 66;
    return 0;
}

static void digest_base_free(digest_base_t* base) {
    for (size_t i = 0; i < base->count; i++) free(base->entries[i].path);
    free(base->entries);
    base->entries = NULL;
    base->count = base->cap = 0;
}

/* Returns 0, -1 (top/schema failure) or -2 (entry failure). */
static int digest_base_entry(digest_json_t* j, digest_base_t* base) {
    if (djs_expect(j, '{')) return -2;
    if (djs_ws(j) && *j->p == '}') return -2;
    digest_entry_t e;
    memset(&e, 0, sizeof(e));
    e.kind = -1;
    e.language = -1;
    int seen_path = 0, seen_kind = 0, seen_lang = 0;
    int seen_hash = 0, seen_bytes = 0, seen_lines = 0;
    int rc = -2;
    for (;;) {
        char* key = NULL;
        size_t keylen = 0;
        if (djs_string(j, &key, &keylen) || djs_expect(j, ':')) {
            free(key);
            goto done;
        }
        if (!strcmp(key, "path")) {
            if (seen_path) { free(key); goto done; }
            seen_path = 1;
            size_t n = 0;
            if (djs_path(j, &e.path, &n) || !n || n > DIGEST_MAX_PATH - 1) {
                free(key);
                goto done;
            }
        } else if (!strcmp(key, "kind")) {
            if (seen_kind) { free(key); goto done; }
            seen_kind = 1;
            char* s = NULL;
            size_t n = 0;
            int kind = -1;
            if (djs_string(j, &s, &n)) { free(key); goto done; }
            for (size_t i = 0; i < sizeof(digest_kind_names) / sizeof(*digest_kind_names); i++)
                if (!strcmp(s, digest_kind_names[i])) kind = (int)i;
            free(s);
            if (kind < 0) { free(key); goto done; }
            e.kind = kind;
        } else if (!strcmp(key, "language")) {
            if (seen_lang) { free(key); goto done; }
            seen_lang = 1;
            if (!djs_ws(j)) { free(key); goto done; }
            if (*j->p == 'n') {
                if (djs_lit(j, "null")) { free(key); goto done; }
                e.language = L_NONE;
            } else {
                char* s = NULL;
                size_t n = 0;
                int lang = -1;
                if (djs_string(j, &s, &n)) { free(key); goto done; }
                for (int i = 0; i < L_NONE; i++)
                    if (!strcmp(s, digest_languages[i])) lang = i;
                free(s);
                if (lang < 0) { free(key); goto done; }
                e.language = lang;
            }
        } else if (!strcmp(key, "blake3")) {
            if (seen_hash) { free(key); goto done; }
            seen_hash = 1;
            if (digest_hex_hash(j, e.hash)) { free(key); goto done; }
        } else if (!strcmp(key, "bytes")) {
            if (seen_bytes) { free(key); goto done; }
            seen_bytes = 1;
            uint64_t v = 0;
            if (djs_uint(j, &v)) { free(key); goto done; }
            e.bytes = (unsigned long)v;
        } else if (!strcmp(key, "lines")) {
            if (seen_lines) { free(key); goto done; }
            seen_lines = 1;
            uint64_t v = 0;
            if (djs_uint(j, &v)) { free(key); goto done; }
            e.lines = (unsigned long)v;
        } else { free(key); goto done; }
        free(key);
        if (!djs_ws(j)) goto done;
        if (*j->p == ',') { j->p++; continue; }
        if (*j->p == '}') { j->p++; break; }
        goto done;
    }
    if (!seen_path || !seen_kind || !seen_lang) goto done;
    if (e.kind == D_DIR) {
        if (seen_hash || seen_bytes || seen_lines) goto done;
    } else if (!seen_hash || !seen_bytes || !seen_lines) goto done;
    if (base->count >= DIGEST_MAX_ENTRIES) goto done;
    if (base->count == base->cap) {
        size_t ncap = base->cap ? base->cap * 2 : 64;
        digest_entry_t* next = realloc(base->entries, ncap * sizeof(*next));
        if (!next) goto done;
        base->entries = next;
        base->cap = ncap;
    }
    base->entries[base->count++] = e;
    return 0;
done:
    free(e.path);
    return rc;
}

static int digest_manifest_parse(const char* data, size_t len, digest_base_t* base) {
    digest_json_t j = {(const unsigned char*)data, (const unsigned char*)data + len};
    memset(base, 0, sizeof(*base));
    int seen_schema = 0, seen_generator = 0, seen_root = 0, seen_fp = 0, seen_entries = 0;
    int rc = -1;
    if (djs_expect(&j, '{')) goto done;
    if (djs_ws(&j) && *j.p == '}') goto done;
    for (;;) {
        char* key = NULL;
        size_t keylen = 0;
        if (djs_string(&j, &key, &keylen) || djs_expect(&j, ':')) {
            free(key);
            goto done;
        }
        if (!strcmp(key, "schema")) {
            if (seen_schema) { free(key); goto done; }
            seen_schema = 1;
            char* s = NULL;
            size_t n = 0;
            int ok = !djs_string(&j, &s, &n) && !strcmp(s, "jash.project-observation.v1");
            free(s);
            if (!ok) { free(key); goto done; }
        } else if (!strcmp(key, "generator")) {
            if (seen_generator) { free(key); goto done; }
            seen_generator = 1;
            char* s = NULL;
            size_t n = 0;
            int ok = !djs_string(&j, &s, &n) && !strcmp(s, DIGEST_GENERATOR_VERSION);
            free(s);
            if (!ok) { free(key); goto done; }
        } else if (!strcmp(key, "root")) {
            if (seen_root) { free(key); goto done; }
            seen_root = 1;
            char* s = NULL;
            size_t n = 0;
            if (djs_path(&j, &s, &n)) { free(key); goto done; }
            free(s);
        } else if (!strcmp(key, "fingerprint")) {
            if (seen_fp) { free(key); goto done; }
            seen_fp = 1;
            unsigned char raw[BLAKE3_OUT_LEN];
            if (digest_hex_hash(&j, raw)) {
                free(key);
                goto done;
            }
            digest_hash_hex(raw, base->base_fp);
        } else if (!strcmp(key, "entries")) {
            if (seen_entries) { free(key); goto done; }
            seen_entries = 1;
            if (djs_expect(&j, '[')) { free(key); goto done; }
            if (djs_ws(&j) && *j.p == ']') { j.p++; }
            else {
                for (;;) {
                    int erc = digest_base_entry(&j, base);
                    if (erc) { free(key); rc = -2; goto done; }
                    if (!djs_ws(&j)) { free(key); rc = -2; goto done; }
                    if (*j.p == ',') { j.p++; continue; }
                    if (*j.p == ']') { j.p++; break; }
                    free(key);
                    rc = -2;
                    goto done;
                }
            }
        } else if (djs_skip(&j)) {
            free(key);
            goto done;
        }
        free(key);
        if (!djs_ws(&j)) goto done;
        if (*j.p == ',') { j.p++; continue; }
        if (*j.p == '}') { j.p++; break; }
        goto done;
    }
    if (!seen_schema || !seen_generator || !seen_root || !seen_fp || !seen_entries) goto done;
    if (djs_ws(&j) || j.p != j.end) goto done;
    qsort(base->entries, base->count, sizeof(*base->entries), digest_entry_cmp);
    for (size_t i = 1; i < base->count; i++)
        if (!strcmp(base->entries[i-1].path, base->entries[i].path)) {
            rc = -2;
            goto done;
        }
    return 0;
done:
    if (rc == 0) rc = -1;
    digest_base_free(base);
    return rc;
}

static int digest_path_cmp(const void* key, const void* elem) {
    return strcmp((const char*)key, ((const digest_entry_t*)elem)->path);
}

/* Renders jash.project-diff.v1: added/modified/removed records sorted by
 * path. Same-path kind changes surface as a removed+added pair; directories
 * never appear in modified (they carry no content hash). */
static int digest_diff_render(const char* root, digest_inventory_t* inv,
                              const char* fingerprint, const char* base_path,
                              digest_buffer_t* b) {
    int fd = open(base_path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return digest_error("cannot open diff base without following symlinks");
    struct stat st;
    char* data = NULL;
    digest_base_t base;
    memset(&base, 0, sizeof(base));
    int rc = 1;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
        digest_error("diff base is not a regular file");
        goto done;
    }
    if (st.st_size < 1 || (uint64_t)st.st_size > DIGEST_DIFF_MAX_BASE_BYTES) {
        digest_error("diff base size out of bounds (1..32 MiB)");
        goto done;
    }
    data = malloc((size_t)st.st_size + 1);
    if (!data) { digest_error("diff base allocation failed"); goto done; }
    size_t off = 0;
    for (;;) {
        ssize_t n = read(fd, data + off, (size_t)st.st_size - off);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { digest_error("diff base read failed"); goto done; }
        if (!n) break;
        off += (size_t)n;
    }
    if (off != (uint64_t)st.st_size) { digest_error("diff base changed during read"); goto done; }
    data[off] = '\0';
    int prc = digest_manifest_parse(data, off, &base);
    if (prc == -2) { digest_error("diff base has a corrupt entry"); goto done; }
    if (prc) { digest_error("diff base is not a jash.project-observation.v1 manifest"); goto done; }
    jash_json_writer_t json;
    jash_jsonw_init(&json, digest_buffer_writer, b);
    jash_jsonw_raw(&json, "{\"schema\":\"jash.project-diff.v1\",\"generator\":");
    jash_jsonw_string(&json, DIGEST_GENERATOR_VERSION);
    jash_jsonw_raw(&json, ",\"root\":"); jash_jsonw_string_or_bytes(&json, root);
    jash_jsonw_raw(&json, ",\"base_fingerprint\":"); jash_jsonw_string(&json, base.base_fp);
    jash_jsonw_raw(&json, ",\"fingerprint\":"); jash_jsonw_string(&json, fingerprint);
    jash_jsonw_raw(&json, ",\"added\":[");
    int first = 1;
    for (size_t i = 0; i < inv->count; i++) {
        digest_entry_t* e = &inv->entries[i];
        digest_entry_t* old = bsearch(e->path, base.entries, base.count,
                                      sizeof(*base.entries), digest_path_cmp);
        if (old && old->kind == e->kind) continue;
        /* A same-path kind change lands here; its removal is reported below. */
        if (!first) jash_jsonw_raw(&json, ",");
        first = 0;
        digest_diff_record(&json, e);
    }
    jash_jsonw_raw(&json, "],\"modified\":[");
    first = 1;
    for (size_t i = 0; i < inv->count; i++) {
        digest_entry_t* e = &inv->entries[i];
        digest_entry_t* old = bsearch(e->path, base.entries, base.count,
                                      sizeof(*base.entries), digest_path_cmp);
        if (!old || old->kind != e->kind || e->kind == D_DIR) continue;
        if (!memcmp(old->hash, e->hash, sizeof(e->hash))) continue;
        char oldhex[65];
        digest_hash_hex(old->hash, oldhex);
        if (!first) jash_jsonw_raw(&json, ",");
        first = 0;
        digest_diff_record_trailer(&json, e, oldhex);
    }
    jash_jsonw_raw(&json, "],\"removed\":[");
    first = 1;
    for (size_t i = 0; i < base.count; i++) {
        digest_entry_t* e = &base.entries[i];
        digest_entry_t* live = bsearch(e->path, inv->entries, inv->count,
                                       sizeof(*inv->entries), digest_path_cmp);
        if (live && live->kind == e->kind) continue;
        if (!first) jash_jsonw_raw(&json, ",");
        first = 0;
        digest_diff_record(&json, e);
    }
    jash_jsonw_raw(&json, "]}");
    jash_jsonw_raw(&json, "\n");
    if (jash_jsonw_finish(&json) || b->failed) {
        digest_error("diff encoding or output limit failed");
        goto done;
    }
    rc = 0;
done:
    if (fd >= 0 && close(fd) < 0 && !rc) rc = digest_error("diff base close failed");
    free(data);
    digest_base_free(&base);
    return rc;
}

/* Open each absolute path component without following symlinks. realpath is
 * used only for the user-selected project root, never for cache paths. */
static int digest_open_dir(const char* path) {
    if (!path || path[0] != '/' || strlen(path) >= DIGEST_MAX_PATH) { errno = EINVAL; return -1; }
    char copy[DIGEST_MAX_PATH];
    /* INV-JASH-TIER1-SOURCE-001: the path was bounded before this copy. */
    memcpy(copy, path, strlen(path) + 1);
    int fd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    char* save = NULL;
    for (char* part = strtok_r(copy, "/", &save); part && fd >= 0; part = strtok_r(NULL, "/", &save)) {
        int next = openat(fd, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        int err = errno;
        close(fd);
        fd = next;
        errno = err;
    }
    return fd;
}

static int digest_private_dir(int parent, const char* name) {
    if (mkdirat(parent, name, 0700) < 0 && errno != EEXIST) return -1;
    int fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_uid != geteuid() || (st.st_mode & 0022)) {
        close(fd); errno = EPERM; return -1;
    }
    return fd;
}

static int digest_cache_open(void) {
    int home = digest_open_dir(getenv("HOME"));
    if (home < 0) return -1;
    struct stat st;
    if (fstat(home, &st) < 0 || st.st_uid != geteuid() || (st.st_mode & 0022)) {
        close(home); errno = EPERM; return -1;
    }
    int jash = digest_private_dir(home, ".jash");
    close(home);
    if (jash < 0) return -1;
    int cache = digest_private_dir(jash, "digest");
    close(jash);
    return cache;
}

static int digest_cache_safe(const struct stat* st) {
    /* A concurrent atomic rename may leave an already-open old inode with
     * st_nlink==0 while the replacement is installed. It is still a regular,
     * owner-only inode; hardlinks (>1) remain refused. */
    return S_ISREG(st->st_mode) && st->st_uid == geteuid() &&
        st->st_nlink <= 1 && !(st->st_mode & 0022);
}

static int digest_cache_target(int dir, const char* name) {
    struct stat st;
    if (fstatat(dir, name, &st, AT_SYMLINK_NOFOLLOW) < 0) return errno == ENOENT ? 0 : -1;
    if (!digest_cache_safe(&st)) { errno = EPERM; return -1; }
    return 0;
}

static void digest_bytes_hash(const char* data, size_t len, char out[65]) {
    struct blake3 h;
    unsigned char hash[BLAKE3_OUT_LEN];
    blake3_init(&h);
    blake3_update(&h, data, len);
    blake3_final(&h, hash, sizeof(hash));
    digest_hash_hex(hash, out);
}

/* 0 hit, 1 absent, -1 unsafe/corrupt/I/O error. Integrity header prevents a
 * truncated or accidentally overwritten cache from masquerading as a digest. */
static int digest_cache_read(int dir, const char* name, const char* key, digest_buffer_t* b) {
    int fd = openat(dir, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return errno == ENOENT ? 1 : -1;
    struct stat before, after;
    int rc = -1;
    if (fstat(fd, &before) < 0 || !digest_cache_safe(&before) || before.st_size < 1 ||
        (uint64_t)before.st_size > DIGEST_MAX_OUTPUT_BYTES) goto done;
    char buf[16384];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) goto done;
        if (!n) break;
        if (digest_buffer_writer(b, buf, (size_t)n) < 0) goto done;
    }
    if (fstat(fd, &after) < 0 || !digest_same_stat(&before, &after) ||
        b->len != (uint64_t)before.st_size) goto done;
    char* nl = memchr(b->data, '\n', b->len);
    if (!nl) goto done;
    size_t header_len = (size_t)(nl + 1 - b->data);
    char hash[65], expected[256];
    digest_bytes_hash(nl + 1, b->len - header_len, hash);
    int n = snprintf(expected, sizeof(expected), "<!-- %s %s %lu %s -->\n",
                     DIGEST_GENERATOR_VERSION, key, (unsigned long)(b->len - header_len), hash);
    if (n < 0 || (size_t)n != header_len || memcmp(b->data, expected, header_len)) goto done;
    b->len -= header_len;
    memmove(b->data, nl + 1, b->len);
    b->data[b->len] = '\0';
    rc = 0;
done:
    if (close(fd) < 0) rc = -1;
    return rc;
}

static int digest_write_all(int fd, const char* data, size_t n) {
    while (n) {
        ssize_t written = write(fd, data, n);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return -1;
        data += written;
        n -= (size_t)written;
    }
    return 0;
}

/* ── Savings ledger (H2) ──────────────────────────────────────────────
 * Every served briefing appends one jash.digest-receipt.v1 line to
 * ledger.jsonl in the cache directory: who (root), what (fingerprint,
 * level), and whether it was served from cache or rendered. Pure modes
 * (fingerprint/manifest/diff/no-cache) never write: the --no-cache posture
 * forbids all cache writes. Telemetry is best-effort and subordinate: a
 * ledger failure never fails the briefing it describes. Past 1 MiB the
 * ledger rotates to ledger.jsonl.1 (one generation, then skipped). */

#define DIGEST_LEDGER_MAX_BYTES (1024u * 1024u)
#define DIGEST_LEDGER_READ_MAX (4u * 1024u * 1024u)

static int digest_deliver(const digest_cfg_t* cfg, const digest_buffer_t* b,
                          jash_writer_t w, void* ctx);

static void digest_ledger_append(int cache, const char* root, const char* level,
                                 const char* fingerprint, int hit,
                                 const digest_inventory_t* inv, size_t bytes_out) {
    struct stat st;
    if (fstatat(cache, "ledger.jsonl", &st, AT_SYMLINK_NOFOLLOW) == 0) {
        if (!digest_cache_safe(&st)) return;
        if ((uint64_t)st.st_size > DIGEST_LEDGER_MAX_BYTES &&
            renameat(cache, "ledger.jsonl", cache, "ledger.jsonl.1") < 0) return;
    } else if (errno != ENOENT) return;
    int fd = openat(cache, "ledger.jsonl",
                    O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return;
    if (fstat(fd, &st) < 0 || !digest_cache_safe(&st)) { close(fd); return; }
    time_t now = time(NULL);
    digest_buffer_t line = {0};
    jash_json_writer_t json;
    jash_jsonw_init(&json, digest_buffer_writer, &line);
    jash_jsonw_raw(&json, "{\"schema\":\"jash.digest-receipt.v1\",\"generator\":");
    jash_jsonw_string(&json, DIGEST_GENERATOR_VERSION);
    jash_jsonw_raw(&json, ",\"ts\":");
    jash_jsonw_u64(&json, now < 0 ? 0 : (uint64_t)now);
    jash_jsonw_raw(&json, ",\"root\":");
    jash_jsonw_string_or_bytes(&json, root);
    jash_jsonw_raw(&json, ",\"level\":");
    jash_jsonw_string(&json, level);
    jash_jsonw_raw(&json, ",\"fingerprint\":");
    jash_jsonw_string(&json, fingerprint);
    jash_jsonw_raw(&json, ",\"served_from_cache\":");
    jash_jsonw_raw(&json, hit ? "true" : "false");
    jash_jsonw_raw(&json, ",\"bytes_out\":");
    jash_jsonw_u64(&json, (uint64_t)bytes_out);
    jash_jsonw_raw(&json, ",\"files\":");
    jash_jsonw_u64(&json, (uint64_t)(inv->count - inv->counts[D_DIR]));
    jash_jsonw_raw(&json, ",\"directories\":");
    jash_jsonw_u64(&json, (uint64_t)inv->counts[D_DIR]);
    jash_jsonw_raw(&json, ",\"lines\":");
    jash_jsonw_u64(&json, (uint64_t)inv->lines);
    jash_jsonw_raw(&json, "}\n");
    if (!jash_jsonw_finish(&json) && !line.failed)
        digest_write_all(fd, line.data, line.len);
    free(line.data);
    close(fd);
}

/* Open the digest cache directory without creating anything: read-only
 * queries must not mkdir on a fresh machine. Returns -1 with errno set. */
static int digest_cache_open_ro(void) {
    int home = digest_open_dir(getenv("HOME"));
    if (home < 0) return -1;
    struct stat st;
    if (fstat(home, &st) < 0 || st.st_uid != geteuid() || (st.st_mode & 0022)) {
        close(home);
        errno = EPERM;
        return -1;
    }
    int jash = openat(home, ".jash", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    close(home);
    if (jash < 0) return -1;
    int cache = openat(jash, "digest", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    close(jash);
    return cache;
}

int digest_ledger_print(long count, const char* output_path,
                        jash_writer_t w, void* ctx) {
    if (count < 1 || count > 1000) return digest_error("invalid ledger count");
    if (output_path && strlen(output_path) >= DIGEST_MAX_PATH)
        return digest_error("output path exceeds 4095 bytes");
    digest_buffer_t body = {0};
    int cache = digest_cache_open_ro();
    int rc = 1;
    if (cache < 0 && errno == ENOENT) rc = 0;
    else if (cache < 0) digest_error("cannot open digest cache directory");
    else {
        int fd = openat(cache, "ledger.jsonl", O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0 && errno == ENOENT) rc = 0;
        else if (fd < 0) digest_error("cannot open digest ledger");
        else {
            struct stat st;
            char* data = NULL;
            if (fstat(fd, &st) < 0) digest_error("cannot stat digest ledger");
            else if (!digest_cache_safe(&st)) digest_error("unsafe digest ledger");
            else if ((uint64_t)st.st_size > DIGEST_LEDGER_READ_MAX)
                digest_error("digest ledger exceeds 4 MiB");
            else if (st.st_size > 0) {
                data = malloc((size_t)st.st_size);
                size_t off = 0;
                int ok = data != NULL;
                while (ok && off < (size_t)st.st_size) {
                    ssize_t n = read(fd, data + off, (size_t)st.st_size - off);
                    if (n < 0 && errno == EINTR) continue;
                    if (n <= 0) { ok = 0; break; }
                    off += (size_t)n;
                }
                if (!ok || off != (size_t)st.st_size) digest_error("digest ledger read failed");
                else {
                    size_t end = off;
                    while (end > 0 && data[end-1] == '\n') end--;
                    size_t starts[1000];
                    size_t nlines = 0, p = 0, line_start = 0;
                    while (p < end) {
                        if (data[p] == '\n') {
                            starts[nlines % (size_t)count] = line_start;
                            nlines++;
                            line_start = p + 1;
                        }
                        p++;
                    }
                    if (line_start < end) {
                        starts[nlines % (size_t)count] = line_start;
                        nlines++;
                    }
                    size_t keep = nlines < (size_t)count ? nlines : (size_t)count;
                    size_t first = nlines - keep;
                    for (size_t k = 0; k < keep; k++) {
                        size_t s = starts[(first + k) % (size_t)count];
                        size_t e = s;
                        while (e < end && data[e] != '\n') e++;
                        if (digest_buffer_writer(&body, data + s, e - s) < 0 ||
                            digest_buffer_writer(&body, "\n", 1) < 0) break;
                    }
                    if (!body.failed) rc = 0;
                    else digest_error("ledger encoding failed");
                }
                free(data);
            } else rc = 0;
            if (close(fd) < 0 && !rc) rc = digest_error("digest ledger close failed");
        }
        if (close(cache) < 0 && !rc) rc = digest_error("cache directory close failed");
    }
    digest_cfg_t dcfg;
    memset(&dcfg, 0, sizeof(dcfg));
    dcfg.output_path = output_path;
    if (!rc) rc = digest_deliver(&dcfg, &body, w, ctx);
    free(body.data);
    return rc;
}

int digest_cache_status_print(const char* output_path,
                              jash_writer_t w, void* ctx) {
    if (output_path && strlen(output_path) >= DIGEST_MAX_PATH)
        return digest_error("output path exceeds 4095 bytes");
    unsigned long entries = 0, bytes = 0;
    uint64_t oldest = 0, newest = 0;
    int cache = digest_cache_open_ro();
    int rc = 1;
    if (cache < 0 && errno == ENOENT) rc = 0;
    else if (cache < 0) digest_error("cannot open digest cache directory");
    else {
        rc = 0;
        int dir = dup(cache);
        if (dir < 0) digest_error("cannot read digest cache directory");
        else {
            DIR* d = fdopendir(dir);
            if (!d) {
                close(dir);
                digest_error("cannot read digest cache directory");
            } else {
                for (;;) {
                    errno = 0;
                    struct dirent* de = readdir(d);
                    if (!de) {
                        if (errno) rc = digest_error("digest cache read failed");
                        break;
                    }
                    size_t n = strlen(de->d_name);
                    if (n < 4 || strcmp(de->d_name + n - 3, ".md") ||
                        de->d_name[0] == '.') continue;
                    struct stat st;
                    if (fstatat(cache, de->d_name, &st, AT_SYMLINK_NOFOLLOW) < 0) {
                        if (errno == ENOENT) continue;
                        rc = digest_error("cannot stat digest cache entry");
                        break;
                    }
                    if (!S_ISREG(st.st_mode)) continue;
                    entries++;
                    bytes += (unsigned long)st.st_size;
                    uint64_t mt = (uint64_t)st.st_mtime;
                    if (!oldest || mt < oldest) oldest = mt;
                    if (mt > newest) newest = mt;
                }
                /* closedir failure after a scan error keeps the first error. */
                if (closedir(d) < 0 && !rc) rc = digest_error("digest cache close failed");
            }
        }
        if (close(cache) < 0 && !rc) rc = digest_error("cache directory close failed");
    }
    digest_buffer_t body = {0};
    if (!rc) {
        jash_json_writer_t json;
        jash_jsonw_init(&json, digest_buffer_writer, &body);
        jash_jsonw_raw(&json, "{\"schema\":\"jash.digest-cache-status.v1\",\"generator\":");
        jash_jsonw_string(&json, DIGEST_GENERATOR_VERSION);
        jash_jsonw_raw(&json, ",\"entries\":");
        jash_jsonw_u64(&json, entries);
        jash_jsonw_raw(&json, ",\"bytes\":");
        jash_jsonw_u64(&json, bytes);
        jash_jsonw_raw(&json, ",\"oldest\":");
        jash_jsonw_u64(&json, oldest);
        jash_jsonw_raw(&json, ",\"newest\":");
        jash_jsonw_u64(&json, newest);
        jash_jsonw_raw(&json, "}\n");
        if (jash_jsonw_finish(&json) || body.failed) {
            digest_error("cache status encoding failed");
            rc = 1;
        }
    }
    digest_cfg_t dcfg;
    memset(&dcfg, 0, sizeof(dcfg));
    dcfg.output_path = output_path;
    if (!rc) rc = digest_deliver(&dcfg, &body, w, ctx);
    free(body.data);
    return rc;
}

/* ── Content-addressed store (H3) ───────────────────────────────────────
 * Successfully read content is staged into cas/tmp/ and renamed to
 * cas/aa/bb... on its BLAKE3 hash, so identical files across projects (and
 * runs) share one object. Population happens only on the normal cached-
 * briefing path: pure modes (fingerprint/manifest/diff/no-cache) never
 * write. Everything here is best-effort and subordinate: a store hiccup
 * disables CAS for the run but never fails the briefing. Total store size
 * is capped at 512 MiB via a once-per-run snapshot. */

#define DIGEST_CAS_MAX_BYTES (512u * 1024u * 1024u)
#define DIGEST_CAS_SNAPSHOT_ENTRIES 65536u

static unsigned long digest_cas_snapshot(int cas) {
    /* A new open-file description keeps repeated quota observations from
     * inheriting the directory offset left by an earlier enumeration. */
    int root = openat(cas, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    unsigned long total = 0, visited = 0;
    int over = 0;
    if (root < 0) return DIGEST_CAS_MAX_BYTES + 1;
    DIR* d = fdopendir(root);
    if (!d) { close(root); return DIGEST_CAS_MAX_BYTES + 1; }
    for (;;) {
        errno = 0;
        struct dirent* shard = readdir(d);
        if (!shard) break;
        if (shard->d_name[0] == '.') continue;
        int shardfd = openat(cas, shard->d_name,
                             O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (shardfd < 0) continue;
        DIR* sd = fdopendir(shardfd);
        if (!sd) { close(shardfd); continue; }
        for (;;) {
            errno = 0;
            struct dirent* de = readdir(sd);
            if (!de) break;
            if (de->d_name[0] == '.') continue;
            if (++visited > DIGEST_CAS_SNAPSHOT_ENTRIES) { over = 1; break; }
            struct stat st;
            if (fstatat(shardfd, de->d_name, &st, AT_SYMLINK_NOFOLLOW) < 0 ||
                !S_ISREG(st.st_mode)) continue;
            total += (unsigned long)st.st_size;
            if (total > DIGEST_CAS_MAX_BYTES) { over = 1; break; }
        }
        closedir(sd);
        if (errno || over) break;
    }
    int read_ok = errno == 0;
    closedir(d);
    if (!read_ok || over) return DIGEST_CAS_MAX_BYTES + 1;
    return total;
}

/* Opens (creating, owner-checked) cas/ and cas/tmp/, sweeps crash-orphaned
 * staging files, and snapshots the quota. Returns 0 with both fds set, or
 * -1 with both at -1 (CAS disabled, briefing unaffected). */
static int digest_cas_setup(int cache, int* dir, int* tmp) {
    *dir = *tmp = -1;
    int cas = digest_private_dir(cache, "cas");
    if (cas < 0) return -1;
    int staging = digest_private_dir(cas, "tmp");
    if (staging < 0) { close(cas); return -1; }
    int rc = -1;
    int sweep = dup(staging);
    if (sweep >= 0) {
        DIR* d = fdopendir(sweep);
        if (d) {
            for (;;) {
                errno = 0;
                struct dirent* de = readdir(d);
                if (!de) break;
                if (de->d_name[0] == '.') continue;
                unlinkat(staging, de->d_name, 0);
            }
            if (errno == 0 && digest_cas_snapshot(cas) <= DIGEST_CAS_MAX_BYTES) rc = 0;
            closedir(d);
        } else close(sweep);
    }
    if (!rc) {
        *dir = cas;
        *tmp = staging;
        return 0;
    }
    close(staging);
    close(cas);
    return -1;
}

int digest_fetch_print(const char* hash, const char* output_path,
                       jash_writer_t w, void* ctx) {
    if (!hash || strlen(hash) != 64) return digest_error("fetch requires a 64-character content hash");
    for (int i = 0; i < 64; i++)
        if (!((hash[i] >= '0' && hash[i] <= '9') ||
              (hash[i] >= 'a' && hash[i] <= 'f')))
            return digest_error("fetch requires lowercase hex");
    if (output_path && strlen(output_path) >= DIGEST_MAX_PATH)
        return digest_error("output path exceeds 4095 bytes");
    digest_buffer_t body = {0};
    int cache = digest_cache_open_ro();
    int rc = 1;
    if (cache < 0) digest_error("unknown content hash");
    else {
        char shard[3] = {hash[0], hash[1], '\0'};
        int dir = openat(cache, "cas", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        int fd = -1, shardfd = -1;
        struct stat st;
        if (dir < 0) digest_error("unknown content hash");
        else {
            shardfd = openat(dir, shard, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (shardfd < 0) {
                if (errno != ENOENT) digest_error("cannot open digest object");
                else digest_error("unknown content hash");
            } else {
                fd = openat(shardfd, hash + 2, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
                if (fd < 0) {
                    if (errno != ENOENT) digest_error("cannot open digest object");
                    else digest_error("unknown content hash");
                } else if (fstat(fd, &st) < 0 || !digest_cache_safe(&st)) {
                    digest_error("unsafe digest object");
                } else if ((uint64_t)st.st_size > DIGEST_MAX_FILE_BYTES) {
                    digest_error("digest object exceeds 8 MiB");
                } else if (st.st_size > 0) {
                    char buf[16384];
                    for (;;) {
                        ssize_t n = read(fd, buf, sizeof(buf));
                        if (n < 0 && errno == EINTR) continue;
                        if (n < 0) { digest_error("digest object read failed"); break; }
                        if (!n) { rc = 0; break; }
                        if (digest_buffer_writer(&body, buf, (size_t)n) < 0) break;
                    }
                    if (body.failed) digest_error("digest object exceeds output bound");
                } else rc = 0;
                if (fd >= 0 && close(fd) < 0 && !rc) rc = digest_error("digest object close failed");
            }
            if (shardfd >= 0 && close(shardfd) < 0 && !rc)
                rc = digest_error("digest store close failed");
        }
        if (dir >= 0 && close(dir) < 0 && !rc) rc = digest_error("digest store close failed");
        if (close(cache) < 0 && !rc) rc = digest_error("cache directory close failed");
    }
    digest_cfg_t dcfg;
    memset(&dcfg, 0, sizeof(dcfg));
    dcfg.output_path = output_path;
    if (!rc) rc = digest_deliver(&dcfg, &body, w, ctx);
    free(body.data);
    return rc;
}

static int digest_cache_publish(int dir, const char* name, const char* key, const digest_buffer_t* b) {
    char header[256], hash[65], temp[96];
    digest_bytes_hash(b->data, b->len, hash);
    int n = snprintf(header, sizeof(header), "<!-- %s %s %lu %s -->\n",
                     DIGEST_GENERATOR_VERSION, key, (unsigned long)b->len, hash);
    if (n < 0 || (size_t)n >= sizeof(header) || b->len > DIGEST_MAX_OUTPUT_BYTES - (size_t)n) return -1;
    static _Atomic unsigned serial;
    int fd = -1;
    for (int i = 0; i < 128; i++) {
        snprintf(temp, sizeof(temp), ".digest-%ld-%u.tmp", (long)getpid(), atomic_fetch_add(&serial, 1));
        fd = openat(dir, temp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd >= 0 || errno != EEXIST) break;
    }
    if (fd < 0) return -1;
    int rc = digest_write_all(fd, header, (size_t)n);
    if (!rc) rc = digest_write_all(fd, b->data, b->len);
    if (!rc) rc = fsync(fd);
    if (close(fd) < 0) rc = -1;
    if (!rc) rc = digest_cache_target(dir, name);
    if (!rc) rc = renameat(dir, temp, dir, name);
    if (!rc) rc = fsync(dir);
    if (rc) unlinkat(dir, temp, 0);
    return rc;
}

static int digest_deliver(const digest_cfg_t* cfg, const digest_buffer_t* b,
                           jash_writer_t w, void* ctx) {
    if (cfg->output_path) {
        /* Check regular-file type before truncating; a FIFO must never block. */
        int fd = open(cfg->output_path, O_WRONLY | O_CREAT | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0644);
        if (fd < 0) return digest_error("cannot open output file");
        struct stat st;
        int rc = fstat(fd, &st);
        if (!rc && !S_ISREG(st.st_mode)) rc = -1;
        if (!rc) rc = ftruncate(fd, 0);
        if (!rc) rc = digest_write_all(fd, b->data, b->len);
        if (close(fd) < 0) rc = -1;
        return rc ? digest_error("output file write failed") : 0;
    }
    size_t off = 0;
    while (off < b->len) {
        long n = w(ctx, b->data + off, b->len - off);
        if (n <= 0 || (size_t)n > b->len - off) return digest_error("output writer failed");
        off += (size_t)n;
    }
    return 0;
}

int digest_run(const digest_cfg_t* cfg, jash_writer_t w, void* ctx) {
    if (!cfg || !cfg->root_dir || !*cfg->root_dir || !w ||
        cfg->level < DIGEST_BRIEF || cfg->level > DIGEST_DEEP ||
        cfg->jobs < 0 || cfg->jobs > DIGEST_MAX_JOBS || cfg->fetch_hash ||
        (cfg->diff_base && (!*cfg->diff_base || cfg->manifest || cfg->fingerprint)))
        return digest_error("invalid project configuration");
    char* root = realpath(cfg->root_dir, NULL);
    if (!root) return digest_error("cannot resolve project root");
    int fd = digest_open_dir(root);
    if (fd < 0) { free(root); return digest_error("cannot open project root directory"); }
    digest_inventory_t inv = {.readme_rank = 100};
    digest_buffer_t body = {0};
    digest_pool_t pool;
    int cache = -1, rc = 1, hit = 0;
    int workers = cfg->jobs > 0 ? cfg->jobs : (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (workers < 1) workers = 1;
    if (cfg->jobs <= 0 && workers > 8) workers = 8;
    digest_pool_init(&pool, workers);
    inv.entries = calloc(DIGEST_MAX_ENTRIES, sizeof(*inv.entries));
    if (!inv.entries) { close(fd); digest_error("inventory allocation failed"); goto done; }
    /* The briefing path owns the cache (and the content store inside it);
     * pure modes never open either. Opening early lets workers stage CAS
     * objects during the walk; a failed walk leaves empty dirs at most. */
    int want_briefing = !cfg->diff_base && !cfg->manifest && !cfg->fingerprint;
    if (want_briefing && !cfg->no_cache) {
        cache = digest_cache_open();
        if (cache < 0) {
            close(fd);
            digest_error("cannot open safe cache directories (use --no-cache for read-only mode)");
            goto done;
        }
        int cas_dir = -1, cas_tmp = -1;
        pool.cas_ok = digest_cas_setup(cache, &cas_dir, &cas_tmp) == 0;
        pool.cas_dir = cas_dir;
        pool.cas_tmp = cas_tmp;
    }
    int wrc = digest_walk(fd, "", 0, &inv, &pool);
    digest_pool_finish(&pool, wrc);
    /* A walk error already reported itself; a worker abort reports the first
     * staged failure here, after every worker joined. */
    if (wrc && !pool.worker_abort) goto done;
    if (digest_inventory_settle(&inv) || wrc) goto done;
    char fingerprint[65];
    digest_fingerprint(&inv, root, fingerprint);
    if (cfg->manifest && cfg->fingerprint) { digest_error("manifest and fingerprint are mutually exclusive"); goto done; }
    if (cfg->diff_base) {
        if (digest_diff_render(root, &inv, fingerprint, cfg->diff_base, &body)) goto done;
    } else if (cfg->manifest) {
        if (digest_manifest(root, &inv, fingerprint, &body)) goto done;
    } else if (cfg->fingerprint) {
        jash_writef(digest_buffer_writer, &body, "%s\n", fingerprint);
        if (body.failed) { digest_error("fingerprint allocation failed"); goto done; }
    } else {
        char key[65], name[80];
        struct blake3 h;
        unsigned char hash[BLAKE3_OUT_LEN];
        blake3_init(&h);
        digest_hash_field(&h, fingerprint, 64);
        const char* level = digest_level_name(cfg->level);
        digest_hash_field(&h, level, strlen(level));
        blake3_final(&h, hash, sizeof(hash));
        digest_hash_hex(hash, key);
        snprintf(name, sizeof(name), "%s.md", key);
        hit = 0;
        if (cache >= 0) {
            if (digest_cache_target(cache, name) < 0) { digest_error("unsafe cache entry"); goto done; }
            if (!cfg->refresh) {
                int result = digest_cache_read(cache, name, key, &body);
                if (result < 0) { digest_error("unsafe, corrupt or unreadable cache entry"); goto done; }
                hit = result == 0;
            }
        }
        if (!hit) {
            if (digest_compose(cfg, root, &inv, fingerprint, &body)) goto done;
            if (cache >= 0 && digest_cache_publish(cache, name, key, &body) < 0) {
                digest_error("atomic cache publication failed"); goto done;
            }
        }
    }
    rc = digest_deliver(cfg, &body, w, ctx);
    /* Served briefings earn a savings receipt; pure modes never reach here
     * with an open cache, so the --no-cache posture holds. */
    if (!rc && !cfg->diff_base && cache >= 0)
        digest_ledger_append(cache, root, digest_level_name(cfg->level),
                             fingerprint, hit, &inv, body.len);
done:
    /* The pool is initialized before cache setup so its workers can feed CAS
     * during the walk. Cache-open/allocation failures can jump here before the
     * normal join; finish is idempotent once a successful walk already joined. */
    digest_pool_finish(&pool, 1);
    /* Subordinate store fds close quietly: they must never fail the briefing. */
    if (pool.cas_tmp >= 0) close(pool.cas_tmp);
    if (pool.cas_dir >= 0) close(pool.cas_dir);
    if (cache >= 0 && close(cache) < 0 && !rc) rc = digest_error("cache directory close failed");
    for (size_t i = 0; i < inv.count; i++) {
        free(inv.entries[i].path);
        free(inv.entries[i].capture);
    }
    free(inv.entries);
    free(body.data);
    free(root);
    return rc;
}

/* ── Session digest ───────────────────────────────────────────────────── */

#define SES_MAX_CMD 256
#define SES_LINE_LEN 4096

typedef struct {
    unsigned long long seq;
    unsigned long long ts_sec;
    int  rc;
    char tag[32];
    char cmd[SES_MAX_CMD];
} ses_rec_t;

static int ses_read_chain(ses_rec_t* buf, int max, int* total_out) {
    const char* home = getenv("HOME");
    if (!home || !*home) return -1;
    char path[520];
    snprintf(path, sizeof(path), "%s/.jash/chain.log", home);
    FILE* f = fopen(path, "r");
    if (!f) return -1;

    int total = 0;
    char line[SES_LINE_LEN];
    while (fgets(line, sizeof(line), f)) {
        unsigned long long seq = 0, ts_sec = 0, ts_ns = 0;
        char hash[80];
        if (sscanf(line, "%llu %llu.%llu %79s", &seq, &ts_sec, &ts_ns, hash) < 4) continue;
        const char* cp = strstr(line, " cmd=");
        if (!cp) continue;
        const char* cmd = cp + 5;
        if (strncmp(cmd, "SESSION_END", 11) == 0) continue;
        int rc = 0;
        const char* rp = strstr(line, " rc=");
        if (rp) sscanf(rp, " rc=%d", &rc);

        char tag[32] = "-";
        const char* tp = strstr(line, " tag=");
        if (tp) {
            tp += 5;
            size_t ti = 0;
            while (*tp && *tp != ' ' && *tp != '\n' && ti < sizeof(tag) - 1) tag[ti++] = *tp++;
            tag[ti] = '\0';
        }

        if (total < max) {
            ses_rec_t* r = &buf[total];
            r->seq = seq; r->ts_sec = ts_sec; r->rc = rc;
            snprintf(r->tag, sizeof(r->tag), "%s", tag);
            strlcpy(r->cmd, cmd, sizeof(r->cmd));
            r->cmd[sizeof(r->cmd) - 1] = '\0';
            for (int i = 0; r->cmd[i]; i++) if (r->cmd[i] == '\n') { r->cmd[i] = '\0'; break; }
        }
        total++;
    }
    fclose(f);
    if (total_out) *total_out = total;
    return total < max ? total : max;
}

static int ses_git_cmd(const char* cmd, char* out, size_t outsz) {
    FILE* f = popen(cmd, "r");
    if (!f) return -1;
    size_t n = fread(out, 1, outsz - 1, f);
    out[n] = '\0';
    pclose(f);
    return (int)n;
}

static int ses_estimate_tokens(const char* text) {
    if (!text) return 0;
    int tokens = 0, in_word = 0;
    for (const char* p = text; *p; p++) {
        if (*p == ' ' || *p == '\n' || *p == '\t') { in_word = 0; continue; }
        if (!in_word) { tokens++; in_word = 1; }
    }
    return tokens * 2; /* rough: 1 token ≈ 2 words for code */
}

int digest_session_run(const digest_session_cfg_t* cfg, jash_writer_t w, void* ctx) {
    if (!cfg || !w) return 1;

    int max_cmds = 10;
    if (cfg->last_n > 0) max_cmds = cfg->last_n;
    else if (cfg->level == SESH_STANDARD) max_cmds = 25;
    else if (cfg->level == SESH_DEEP) max_cmds = 100;

    ses_rec_t* cmds = (ses_rec_t*)calloc((size_t)max_cmds, sizeof(ses_rec_t));
    if (!cmds) return 1;

    int total = 0;
    int have = ses_read_chain(cmds, max_cmds, &total);

    jash_writes(w, ctx, "# Session Digest\n\n");
    jash_writef(w, ctx, "**level:** %s  \n",
                cfg->level == SESH_BRIEF ? "brief" :
                cfg->level == SESH_DEEP ? "deep" : "standard");
    jash_writef(w, ctx, "**total commands:** %d  \n\n", total);

    /* ── Git status ── */
    if (cfg->include_git) {
        jash_writes(w, ctx, "## Git Status\n\n");
        char gitbuf[8192];
        if (ses_git_cmd("git rev-parse --abbrev-ref HEAD 2>/dev/null", gitbuf, sizeof(gitbuf)) > 0) {
            char* nl = strchr(gitbuf, '\n');
            if (nl) *nl = '\0';
            jash_writef(w, ctx, "**branch:** %s  \n", gitbuf);
        }
        if (ses_git_cmd("git status --porcelain 2>/dev/null | wc -l", gitbuf, sizeof(gitbuf)) > 0) {
            char* nl = strchr(gitbuf, '\n');
            if (nl) *nl = '\0';
            jash_writef(w, ctx, "**dirty files:** %s  \n", gitbuf);
        }
        if (ses_git_cmd("git diff --stat 2>/dev/null", gitbuf, sizeof(gitbuf)) > 0 && gitbuf[0]) {
            jash_writes(w, ctx, "\n```\n");
            jash_writes(w, ctx, gitbuf);
            if (gitbuf[strlen(gitbuf)-1] != '\n') jash_writes(w, ctx, "\n");
            jash_writes(w, ctx, "```\n\n");
        }
    }

    /* ── Recent commands ── */
    jash_writes(w, ctx, "## Recent Commands\n\n");
    if (have > 0) {
        jash_writes(w, ctx, "| # | rc | tag | cmd |\n|---|---|---|---|\n");
        int start = have > max_cmds ? have - max_cmds : 0;
        for (int i = start; i < have; i++) {
            ses_rec_t* r = &cmds[i];
            jash_writef(w, ctx, "| %llu | %d | %s | `%s` |\n",
                        r->seq, r->rc, r->tag, r->cmd);
        }
        jash_writes(w, ctx, "\n");
    } else {
        jash_writes(w, ctx, "(no chain — set JASH_SIGIL>=1)\n\n");
    }

    /* ── Error summary ── */
    if (have > 0) {
        int fails = 0;
        for (int i = 0; i < have; i++) if (cmds[i].rc != 0) fails++;
        jash_writef(w, ctx, "**errors:** %d / %d commands failed (%.0f%%)  \n\n",
                    fails, have, (have > 0) ? (fails * 100.0 / have) : 0.0);
    }

    /* ── Token estimate ── */
    if (have > 0 && cfg->level >= SESH_STANDARD) {
        char tokbuf[8192];
        int tok_total = 0;
        if (ses_git_cmd("git diff --stat 2>/dev/null", tokbuf, sizeof(tokbuf)) > 0)
            tok_total += ses_estimate_tokens(tokbuf);
        ses_rec_t* last = &cmds[have - 1];
        char cmd_text[4096] = {0};
        for (int i = 0; i < have; i++) {
            size_t clen = strlen(cmd_text);
            snprintf(cmd_text + clen, sizeof(cmd_text) - clen, "%s\n", cmds[i].cmd);
        }
        tok_total += ses_estimate_tokens(cmd_text);
        jash_writef(w, ctx, "**estimated tokens:** ~%d  \n", tok_total);
        jash_writes(w, ctx, "\n---\n*Generated by jash `digest --session`*\n");
        (void)last;
    }

    free(cmds);
    return 0;
}

#include "digest_native.inc"
