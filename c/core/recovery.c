// SPDX-License-Identifier: LicenseRef-SPL-1.0
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "recovery.h"
#include "timeline.h"
#include "argv.h"
#include "json_writer.h"
#include "output_budget.h"
#include <builtins/discovery_output.h>
#include <parser/builtin_redir.h>
#include <module/jash_crypto.h>
#include <osif/osinfo.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <dirent.h>

#define REC_MAX_BYTES (16u * 1024u * 1024u)
#define REC_PATH 4096
#define REC_MAGIC "JASHRC01"
#define REC_READY 0u
#define REC_ACTIVE 1u
#define REC_DONE 2u
#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE 1u
#endif

/* Native local-store record, versioned and size-checked; not a portable wire
 * format. The checksum detects damaged records, not malicious same-UID edits. */
typedef struct {
    char magic[8];
    uint32_t record_size, kind, state, reserved;
    uint64_t id, parent_dev, parent_ino, dev, ino, size, mode, uid, gid;
    uint64_t old_mode, old_uid, old_gid, payload_size;
    int64_t mtime_sec, mtime_ns, ctime_sec, ctime_ns;
    char path[REC_PATH];
    unsigned char post_hash[32], pre_hash[32], record_hash[32];
} recovery_record_t;

static const char* coverage(unsigned kind) {
    switch (kind) {
    case JASH_UNDO_FILE_OVERWRITE: return "file_content";
    case JASH_UNDO_FILE_CREATE: case JASH_UNDO_LINK_CREATE: return "created_entry";
    case JASH_UNDO_DIR_CREATE: return "created_directory";
    case JASH_UNDO_DIR_REMOVE: return "empty_directory_mode";
    case JASH_UNDO_META: return "mode_owner";
    case JASH_UNDO_TIMES: return "timestamps";
    case JASH_UNDO_MOVE: return "moved_entry";
    case JASH_UNDO_TRASH: return "trashed_entry";
    default: return NULL;
    }
}

static int write_all(int fd, const void* bytes, size_t size) {
    const char* p = bytes;
    while (size) {
        ssize_t n = write(fd, p, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n; size -= (size_t)n;
    }
    return 0;
}

static int read_all(int fd, void* bytes, size_t size) {
    char* p = bytes;
    while (size) {
        ssize_t n = read(fd, p, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n; size -= (size_t)n;
    }
    return 0;
}

static int make_private_dir(int parent, const char* name) {
    mode_t mask = umask(0);
    int rc = mkdirat(parent, name, 0700);
    int saved = errno;
    umask(mask); errno = saved;
    return rc;
}

static int private_dir(int parent, const char* name, int create) {
    if (create) {
        if (!make_private_dir(parent, name)) { if (fsync(parent)) return -1; }
        else if (errno != EEXIST) return -1;
    }
    int fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    struct stat st;
    if (fd >= 0 && (fstat(fd, &st) || st.st_uid != geteuid() || (st.st_mode & 0022))) {
        close(fd); errno = EPERM; return -1;
    }
    return fd;
}

static int open_store_home(int create, const char* home) {
    if (!home || home[0] != '/') { errno = EINVAL; return -1; }
    int h = open(home, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (h < 0) return -1;
    int j = private_dir(h, ".jash", create);
    close(h);
    if (j < 0) return -1;
    int store = private_dir(j, "recovery-v1", create);
    close(j);
    return store;
}

static int open_store(int create) { return open_store_home(create, getenv("HOME")); }

/* Resolve the parent physically before joining the literal last component.
 * Never lexically collapse symlink/.. or follow a final symlink. */
static int endpoint(const char* path, char out[REC_PATH], char name[REC_PATH]) {
    if (!path || !*path || strlen(path) >= REC_PATH) { errno = ENAMETOOLONG; return -1; }
    char parent[REC_PATH];
    /* INV-JASH-TIER1-SOURCE-001: endpoint components inherit the checked span. */
    size_t n = strlen(path);
    memcpy(parent, path, n + 1);
    while (n > 1 && parent[n - 1] == '/') parent[--n] = 0;
    char* slash = strrchr(parent, '/');
    if (!slash) { memcpy(name, parent, n + 1); parent[0] = '.'; parent[1] = 0; }
    else {
        memcpy(name, slash + 1, strlen(slash + 1) + 1);
        if (slash == parent) slash[1] = 0; else *slash = 0;
    }
    if (!*name || !strcmp(name, ".") || !strcmp(name, "..")) { errno = EINVAL; return -1; }
    char* physical = realpath(parent, NULL);
    if (!physical) return -1;
    int len = snprintf(out, REC_PATH, "%s%s%s", physical, strcmp(physical, "/") ? "/" : "", name);
    int fd = len >= 0 && len < REC_PATH ? open(physical, O_RDONLY | O_DIRECTORY | O_CLOEXEC) : -1;
    free(physical);
    return fd;
}

static void stamp(recovery_record_t* r, const struct stat* st) {
    r->dev = st->st_dev; r->ino = st->st_ino; r->size = st->st_size;
    r->mode = st->st_mode; r->uid = st->st_uid; r->gid = st->st_gid;
#ifdef __APPLE__
    r->mtime_sec = st->st_mtimespec.tv_sec; r->mtime_ns = st->st_mtimespec.tv_nsec;
    r->ctime_sec = st->st_ctimespec.tv_sec; r->ctime_ns = st->st_ctimespec.tv_nsec;
#else
    r->mtime_sec = st->st_mtim.tv_sec; r->mtime_ns = st->st_mtim.tv_nsec;
    r->ctime_sec = st->st_ctim.tv_sec; r->ctime_ns = st->st_ctim.tv_nsec;
#endif
}

int jash_recovery_canonical(const char* path, char* out, size_t capacity) {
    char absolute[REC_PATH], name[REC_PATH];
    int parent = endpoint(path, absolute, name);
    if (parent < 0) return -1;
    close(parent);
    size_t length = strlen(absolute);
    if (length >= capacity) return -1;
    memcpy(out, absolute, length + 1);
    return 0;
}

static int same_stamp(const recovery_record_t* r, const struct stat* st, int captured) {
    recovery_record_t now = {0}; stamp(&now, st);
    return r->dev == now.dev && r->ino == now.ino && r->mode == now.mode &&
        r->uid == now.uid && r->gid == now.gid &&
        (S_ISDIR(st->st_mode) || (r->size == now.size && r->mtime_sec == now.mtime_sec && r->mtime_ns == now.mtime_ns)) &&
        (captured || S_ISDIR(st->st_mode) || (r->ctime_sec == now.ctime_sec && r->ctime_ns == now.ctime_ns));
}

static int hash_file(int fd, uint64_t size, unsigned char digest[32], int copy_to) {
    if (size > REC_MAX_BYTES || lseek(fd, 0, SEEK_SET) < 0) return -1;
    jash_sha256_ctx_t hash; jash_sha256_init(&hash);
    unsigned char buf[65536];
    uint64_t total = 0;
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return -1;
        if (!n) break;
        if ((uint64_t)n > size - total) return -1;
        jash_sha256_update(&hash, buf, (size_t)n);
        if (copy_to >= 0 && write_all(copy_to, buf, (size_t)n)) return -1;
        total += (uint64_t)n;
    }
    if (total != size) return -1;
    jash_sha256_final(&hash, digest);
    return 0;
}

static int inspect_target(int parent, const char* name, recovery_record_t* r, int capture, int moved) {
    struct stat st;
    if (fstatat(parent, name, &st, AT_SYMLINK_NOFOLLOW)) return -1;
    if (!capture && !same_stamp(r, &st, moved)) return -1;
    unsigned char hash[32] = {0};
    if (S_ISREG(st.st_mode)) {
        int fd = openat(parent, name, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
        struct stat opened, after;
        int ok = fd >= 0 && !fstat(fd, &opened) && st.st_dev == opened.st_dev && st.st_ino == opened.st_ino &&
                 !hash_file(fd, (uint64_t)st.st_size, hash, -1) && !fstat(fd, &after);
        if (ok) { recovery_record_t before = {0}; stamp(&before, &st); ok = same_stamp(&before, &after, 0); }
        if (fd >= 0) close(fd);
        if (!ok) return -1;
    } else if (S_ISLNK(st.st_mode)) {
        char link[REC_PATH];
        ssize_t n = readlinkat(parent, name, link, sizeof(link));
        if (n < 0 || (size_t)n >= sizeof(link)) return -1;
        jash_sha256(link, (size_t)n, hash);
    } else if (S_ISDIR(st.st_mode)) {
        int fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0) return -1;
        close(fd);
    } else return -1;
    if (capture) { stamp(r, &st); memcpy(r->post_hash, hash, sizeof(hash)); }
    else if (memcmp(r->post_hash, hash, sizeof(hash))) return -1;
    return 0;
}

static int record_digest(int fd, recovery_record_t* r, unsigned char out[32]) {
    recovery_record_t copy = *r;
    memset(copy.record_hash, 0, sizeof(copy.record_hash));
    jash_sha256_ctx_t hash; jash_sha256_init(&hash);
    jash_sha256_update(&hash, (void*)&copy, sizeof(copy));
    if (lseek(fd, sizeof(copy), SEEK_SET) < 0) return -1;
    unsigned char buf[65536]; uint64_t remaining = r->payload_size;
    while (remaining) {
        size_t n = remaining > sizeof(buf) ? sizeof(buf) : (size_t)remaining;
        if (read_all(fd, buf, n)) return -1;
        jash_sha256_update(&hash, buf, n); remaining -= n;
    }
    jash_sha256_final(&hash, out);
    return 0;
}

static int store_record(int fd, recovery_record_t* r) {
    if (record_digest(fd, r, r->record_hash) || lseek(fd, 0, SEEK_SET) < 0 ||
        write_all(fd, r, sizeof(*r)) || fsync(fd)) return -1;
    return 0;
}

static int random_token(char out[33]) {
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    unsigned char bytes[16];
    int rc = fd < 0 ? -1 : read_all(fd, bytes, sizeof(bytes));
    if (fd >= 0) close(fd);
    if (rc) return -1;
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(bytes); i++) { out[i*2] = hex[bytes[i] >> 4]; out[i*2+1] = hex[bytes[i] & 15]; }
    out[32] = 0;
    return 0;
}

static int token_valid(const char* token);

static const char* record_token(const char* name) {
    if (token_valid(name)) return name;
    if (!strncmp(name, ".gc-", 4) && token_valid(name + 4)) return name + 4;
    if (!strncmp(name, ".pending-", 9) && token_valid(name + 9)) return name + 9;
    return NULL;
}

static int quota_number(const char* text, uint64_t* out) {
    if (!text || !*text) return -1;
    uint64_t value = 0;
    for (; *text; text++) {
        if (*text < '0' || *text > '9' || value > (UINT64_MAX - (unsigned)(*text - '0')) / 10) return -1;
        value = value * 10 + (unsigned)(*text - '0');
    }
    *out = value; return 0;
}

static int quota_guard(int store) {
    int fd = openat(store, ".capacity.lock", O_RDWR | O_CREAT | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600);
    struct stat st;
    if (fd < 0) return -1;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() || st.st_nlink != 1 ||
        (st.st_mode & 0077) || fchmod(fd, 0600) || flock(fd, LOCK_EX)) { close(fd); return -1; }
    return fd;
}

static int quota_limit(int store, uint64_t* limit) {
    *limit = 0;
    int fd = openat(store, ".quota", O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return errno == ENOENT ? 0 : -1;
    struct stat st; char text[32];
    int ok = !fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_uid == geteuid() && !(st.st_mode & 0077) &&
             st.st_size > 0 && (uintmax_t)st.st_size < sizeof(text);
    if (ok) { ok = !read_all(fd, text, (size_t)st.st_size); text[st.st_size] = 0; }
    close(fd);
    if (!ok || memchr(text, 0, (size_t)st.st_size)) return -1;
    if (text[st.st_size - 1] == '\n') text[st.st_size - 1] = 0;
    return quota_number(text, limit);
}

static int quota_usage(int store, uint64_t* used) {
    *used = 0;
    int copy = openat(store, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    DIR* dir = copy >= 0 ? fdopendir(copy) : NULL;
    if (!dir) { if (copy >= 0) close(copy); return -1; }
    int rc = 0;
    for (;;) {
        errno = 0; struct dirent* ent = readdir(dir);
        if (!ent) { if (errno) rc = -1; break; }
        if (!record_token(ent->d_name)) continue;
        struct stat st;
        if (fstatat(store, ent->d_name, &st, AT_SYMLINK_NOFOLLOW)) { if (errno == ENOENT) continue; rc = -1; break; }
        if (!S_ISREG(st.st_mode) || st.st_size < 0 || UINT64_MAX - *used < (uint64_t)st.st_size) { rc = -1; break; }
        *used += (uint64_t)st.st_size;
    }
    closedir(dir); return rc;
}

static int quota_admit(int store, uint64_t additional) {
    uint64_t limit, used;
    if (quota_limit(store, &limit)) return -1;
    if (!limit) return 0;
    if (quota_usage(store, &used) || used > limit || additional > limit - used) { errno = EDQUOT; return -1; }
    return 0;
}

int jash_recovery_capture(uint64_t id, int kind, const char* path, const char* data,
                           jash_recovery_ref_t* out) {
    memset(out, 0, sizeof(*out));
    if (!id || !coverage((unsigned)kind)) return -1;
    recovery_record_t r = {0};
    memcpy(r.magic, REC_MAGIC, 8); r.record_size = sizeof(r); r.kind = (unsigned)kind; r.id = id;
    char name[REC_PATH];
    int moving = kind == JASH_UNDO_MOVE || kind == JASH_UNDO_TRASH;
    char origin[REC_PATH] = "";
    int parent = endpoint(moving ? data : path, r.path, name);
    if (parent < 0) return -1;
    struct stat st;
    int store = -1, fd = -1, backup = -1, rc = -1, created = 0, published = 0, capacity_lock = -1;
    char token[33] = "", temporary[48] = "";
    if (fstat(parent, &st)) goto done;
    r.parent_dev = st.st_dev; r.parent_ino = st.st_ino;
    if (moving) {
        char original_name[REC_PATH];
        int original = endpoint(path, origin, original_name);
        if (original < 0) goto done;
        struct stat original_st, entry;
        int ok = !fstat(original, &original_st) && original_st.st_dev == st.st_dev &&
                 fstatat(original, original_name, &entry, AT_SYMLINK_NOFOLLOW) && errno == ENOENT;
        close(original);
        if (!ok) goto done;
        r.old_mode = original_st.st_dev; r.old_uid = original_st.st_ino;
        r.payload_size = strlen(origin) + 1;
    }
    if (kind == JASH_UNDO_DIR_REMOVE) {
        if (!fstatat(parent, name, &st, AT_SYMLINK_NOFOLLOW) || errno != ENOENT || !data || !*data) goto done;
        char* end; errno = 0; r.old_mode = strtoull(data, &end, 8);
        if (errno || *end || r.old_mode > 07777) goto done;
    } else {
        if (inspect_target(parent, name, &r, 1, 0)) goto done;
        if ((kind == JASH_UNDO_FILE_CREATE || kind == JASH_UNDO_FILE_OVERWRITE) && !S_ISREG(r.mode)) goto done;
        if (kind == JASH_UNDO_DIR_CREATE && !S_ISDIR(r.mode)) goto done;
        if (kind == JASH_UNDO_LINK_CREATE && !S_ISREG(r.mode) && !S_ISLNK(r.mode)) goto done;
        if (kind == JASH_UNDO_META) {
            unsigned long long mode, uid, gid; char extra;
            if (S_ISLNK(r.mode) || !data || sscanf(data, "%llo:%llu:%llu%c", &mode, &uid, &gid, &extra) != 3 ||
                mode > 07777 || uid >= (uid_t)-1 || gid >= (gid_t)-1) goto done;
            r.old_mode = mode; r.old_uid = uid; r.old_gid = gid;
        }
        if (kind == JASH_UNDO_TIMES) {
            long long a, m; unsigned long an, mn; char extra;
            if (!data || sscanf(data, "%lld:%lu:%lld:%lu%c", &a, &an, &m, &mn, &extra) != 4 ||
                an > 999999999 || mn > 999999999) goto done;
            r.old_mode = (uint64_t)a; r.old_uid = (uint64_t)m;
            r.old_gid = ((uint64_t)an << 32) | mn;
        }
    }
    if (kind == JASH_UNDO_FILE_OVERWRITE) {
        if (fstatat(parent, name, &st, AT_SYMLINK_NOFOLLOW) || st.st_nlink != 1) goto done;
        if (!data || !*data) goto done;
        backup = open(data, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
        if (backup < 0 || fstat(backup, &st) || !S_ISREG(st.st_mode) || st.st_size < 0 || st.st_size > REC_MAX_BYTES) goto done;
        /* The legacy backup filename binds the preimage digest. Do not trust a
         * same-sized dedup hit, a replacement FIFO, or an arbitrary path. */
        const char* base = strrchr(data, '/'); base = base ? base + 1 : data;
        if (strlen(base) != 67 || strncmp(base, "bk_", 3) || hash_file(backup, st.st_size, r.pre_hash, -1)) goto done;
        char hex[65]; jash_sha256_hex(r.pre_hash, hex);
        if (strcmp(hex, base + 3)) goto done;
        r.payload_size = st.st_size;
    }
    store = open_store(1);
    if (store < 0) goto done;
    capacity_lock = quota_guard(store);
    if (capacity_lock < 0 || quota_admit(store, sizeof(r) + r.payload_size) || random_token(token)) goto done;
    snprintf(temporary, sizeof(temporary), ".pending-%s", token);
    fd = openat(store, temporary, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    created = fd >= 0;
    if (fd < 0 || fchmod(fd, 0600) || flock(fd, LOCK_EX) || write_all(fd, &r, sizeof(r))) goto done;
    if (backup >= 0) {
        unsigned char copied[32];
        if (hash_file(backup, r.payload_size, copied, fd) || memcmp(copied, r.pre_hash, 32)) goto done;
    }
    if (moving && write_all(fd, origin, (size_t)r.payload_size)) goto done;
    if (store_record(fd, &r) || jash_osif_renameat2(store, temporary, store, token, RENAME_NOREPLACE)) goto done;
    published = 1;
    if (fsync(store)) goto done;
    memcpy(out->token, token, sizeof(token)); out->entry_id = id; out->coverage = coverage(r.kind);
    rc = 0;
done:
    if (fd >= 0) { if (close(fd) && !rc) { rc = -1; memset(out, 0, sizeof(*out)); } }
    /* INV-869: do not join two stack pathnames at the cleanup syscall.
     * Publication selects which name owns the artifact. */
    if (rc && created) {
        int removed;
        if (published) removed = unlinkat(store, token, 0);
        else removed = unlinkat(store, temporary, 0);
        if (!removed) (void)fsync(store);
    }
    if (capacity_lock >= 0) close(capacity_lock);
    if (store >= 0) close(store);
    if (backup >= 0) close(backup);
    close(parent);
    return rc;
}

static int token_valid(const char* token) {
    if (!token || strlen(token) != 32) return 0;
    for (unsigned i = 0; i < 32; i++) if (!((token[i] >= '0' && token[i] <= '9') || (token[i] >= 'a' && token[i] <= 'f'))) return 0;
    return 1;
}

struct jash_recovery_pins {
    char** paths;
    size_t count;
    int complete, admission_lock;
};

static int pin_compare(const void* a, const void* b) {
    return strcmp(*(const char* const*)a, *(const char* const*)b);
}

void jash_recovery_pins_free(jash_recovery_pins_t* pins) {
    if (!pins) return;
    if (pins->admission_lock >= 0) close(pins->admission_lock);
    for (size_t i = 0; i < pins->count; i++) free(pins->paths[i]);
    free(pins->paths); free(pins);
}

int jash_recovery_pins_complete(const jash_recovery_pins_t* pins) {
    return pins && pins->complete;
}

jash_recovery_pins_t* jash_recovery_pins_load_home(const char* home) {
    jash_recovery_pins_t* pins = calloc(1, sizeof(*pins));
    if (!pins) return NULL;
    pins->admission_lock = -1;
    int store = open_store_home(0, home);
    if (store < 0) { pins->complete = errno == ENOENT; return pins; }
    pins->admission_lock = quota_guard(store);
    if (pins->admission_lock < 0) { close(store); return pins; }
    int copy = openat(store, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    DIR* directory = copy >= 0 ? fdopendir(copy) : NULL;
    if (!directory) { if (copy >= 0) close(copy); close(store); return pins; }
    pins->paths = calloc(8192, sizeof(*pins->paths));
    if (!pins->paths) { closedir(directory); close(store); return pins; }
    uint64_t scanned_bytes = 0; unsigned records = 0;
    pins->complete = 1;
    for (;;) {
        errno = 0; struct dirent* entry = readdir(directory);
        if (!entry) { if (errno) pins->complete = 0; break; }
        const char* token = record_token(entry->d_name);
        if (!token) continue;
        if (++records > 4096) { pins->complete = 0; break; }
        int fd = openat(store, entry->d_name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
        recovery_record_t r; struct stat st;
        int valid = fd >= 0 && !fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_uid == geteuid() &&
                    st.st_nlink == 1 && !(st.st_mode & 0077) && !read_all(fd, &r, sizeof(r)) &&
                    !memcmp(r.magic, REC_MAGIC, 8) && r.record_size == sizeof(r) && coverage(r.kind) &&
                    r.state <= REC_DONE && r.payload_size <= REC_MAX_BYTES &&
                    st.st_size == (off_t)(sizeof(r) + r.payload_size) && r.path[0] == '/' && memchr(r.path, 0, sizeof(r.path));
        if (valid) {
            scanned_bytes += sizeof(r) + r.payload_size;
            if (scanned_bytes > 64u * 1024u * 1024u) valid = 0;
            else {
                unsigned char hash[32];
                valid = !record_digest(fd, &r, hash) && !memcmp(hash, r.record_hash, sizeof(hash));
            }
        }
        if (fd >= 0) close(fd);
        if (!valid) { pins->complete = 0; break; }
        if (r.state == REC_DONE && !(r.reserved & 1u)) continue;
        char hold[REC_PATH];
        const char* slash = strrchr(r.path, '/');
        int n = snprintf(hold, sizeof(hold), "%.*s/.jash-undo-%s", (int)(slash - r.path), r.path, token);
        if (n < 0 || (size_t)n >= sizeof(hold)) { pins->complete = 0; break; }
        pins->paths[pins->count] = strdup(r.path);
        if (!pins->paths[pins->count]) { pins->complete = 0; break; }
        pins->count++;
        pins->paths[pins->count] = strdup(hold);
        if (!pins->paths[pins->count]) { pins->complete = 0; break; }
        pins->count++;
    }
    closedir(directory); close(store);
    qsort(pins->paths, pins->count, sizeof(*pins->paths), pin_compare);
    return pins;
}

jash_recovery_pins_t* jash_recovery_pins_load(void) { return jash_recovery_pins_load_home(getenv("HOME")); }

static int pin_matches(const jash_recovery_pins_t* pins, const char* path) {
    size_t low = 0, high = pins->count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        if (strcmp(pins->paths[middle], path) < 0) low = middle + 1; else high = middle;
    }
    if (low < pins->count && !strcmp(pins->paths[low], path)) return 1;
    size_t n = strlen(path);
    char prefix[REC_PATH];
    if (n + 1 >= sizeof(prefix)) return 1;
    memcpy(prefix, path, n); prefix[n++] = '/'; prefix[n] = 0;
    low = 0; high = pins->count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        if (strcmp(pins->paths[middle], prefix) < 0) low = middle + 1; else high = middle;
    }
    return low < pins->count && !strncmp(pins->paths[low], prefix, n);
}

int jash_recovery_pins_contains(const jash_recovery_pins_t* pins, const char* path) {
    if (!pins || !pins->complete) return 1;
    if (!pins->count) return 0;
    char canonical[REC_PATH];
    if (jash_recovery_canonical(path, canonical, sizeof(canonical))) return 1;
    if (pin_matches(pins, canonical)) return 1;
    size_t n = strlen(canonical);
    if (n > 5 && !strcmp(canonical + n - 5, ".meta")) {
        canonical[n - 5] = 0;
        if (pin_matches(pins, canonical)) return 1;
    }
    /* A pinned ancestor directory also protects its descendants. */
    for (;;) {
        char* slash = strrchr(canonical, '/');
        if (!slash) break;
        if (slash == canonical) slash[1] = 0; else *slash = 0;
        const char* key = canonical;
        if (bsearch(&key, pins->paths, pins->count, sizeof(*pins->paths), pin_compare)) return 1;
        if (slash == canonical) break;
    }
    return 0;
}

static int empty_directory(int parent, const char* name) {
    int fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return 0;
    DIR* dir = fdopendir(fd);
    if (!dir) { close(fd); return 0; }
    int empty = 1;
    for (;;) {
        errno = 0; struct dirent* entry = readdir(dir);
        if (!entry) { if (errno) empty = 0; break; }
        if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) { empty = 0; break; }
    }
    closedir(dir); return empty;
}

static int move_noreplace(int from, const char* src, int to, const char* dst) {
    return jash_osif_renameat2(from, src, to, dst, RENAME_NOREPLACE);
}

static int move_origin(int fd, const recovery_record_t* r, char name[REC_PATH], const char** code) {
    char original[REC_PATH], canonical[REC_PATH];
    if (r->payload_size < 2 || r->payload_size > sizeof(original) || lseek(fd, sizeof(*r), SEEK_SET) < 0 ||
        read_all(fd, original, (size_t)r->payload_size) || original[r->payload_size - 1] ||
        strlen(original) + 1 != r->payload_size) { *code = "invalid_origin"; return -1; }
    int parent = endpoint(original, canonical, name);
    struct stat st;
    if (parent < 0 || strcmp(canonical, original) || fstat(parent, &st) ||
        (uint64_t)st.st_dev != r->old_mode || (uint64_t)st.st_ino != r->old_uid) {
        if (parent >= 0) close(parent);
        *code = "origin_parent_changed"; return -1;
    }
    if (!fstatat(parent, name, &st, AT_SYMLINK_NOFOLLOW) || errno != ENOENT) {
        close(parent); *code = "destination_exists"; return -1;
    }
    return parent;
}

/* Reversible capture separates pathname races from destructive operations. A
 * replacement at the original name is never overwritten when putting back. */
static int apply_record(int fd, recovery_record_t* r, int parent, const char* name,
                         const char* token, const char** code, int* changed, char hold_path[REC_PATH]) {
    char hold[64]; snprintf(hold, sizeof(hold), ".jash-undo-%s", token);
    const char* slash = strrchr(r->path, '/');
    int n = snprintf(hold_path, REC_PATH, "%.*s/%s", (int)(slash - r->path), r->path, hold);
    if (n < 0 || n >= REC_PATH) { *code = "path_limit"; return 1; }
    r->state = REC_ACTIVE;
    if (store_record(fd, r)) { *code = "record_sync"; return 1; }
    if (make_private_dir(parent, hold)) { *code = "capture_directory"; return 1; }
    int h = openat(parent, hold, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (h < 0) { *code = "capture_directory"; return 1; }
    int result = 1, captured = 0;
    if (r->kind != JASH_UNDO_DIR_REMOVE) {
        if (move_noreplace(parent, name, h, "original")) { *code = "capture_failed"; goto done; }
        captured = 1; *changed = 1;
        if (inspect_target(h, "original", r, 0, 1)) { *code = "target_changed_during_capture"; goto restore; }
    }
    if (r->kind == JASH_UNDO_FILE_CREATE || r->kind == JASH_UNDO_LINK_CREATE || r->kind == JASH_UNDO_DIR_CREATE) {
        if (unlinkat(h, "original", r->kind == JASH_UNDO_DIR_CREATE ? AT_REMOVEDIR : 0)) { *code = "remove_failed"; goto restore; }
        captured = 0;
    } else if (r->kind == JASH_UNDO_DIR_REMOVE) {
        if (make_private_dir(h, "restored")) { *code = "restore_directory"; goto done; }
        int d = openat(h, "restored", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        int ok = d >= 0 && !fchmod(d, r->old_mode);
        if (d >= 0) close(d);
        if (!ok || move_noreplace(h, "restored", parent, name)) { *code = "publish_directory"; goto done; }
        *changed = 1;
    } else if (r->kind == JASH_UNDO_MOVE || r->kind == JASH_UNDO_TRASH) {
        char original_name[REC_PATH];
        int original = move_origin(fd, r, original_name, code);
        if (original < 0) goto restore;
        if (move_noreplace(h, "original", original, original_name)) {
            close(original); *code = "publish_conflict"; goto restore;
        }
        captured = 0;
        if (fsync(original)) { close(original); *code = "origin_sync"; goto done; }
        close(original);
    } else if (r->kind == JASH_UNDO_TIMES) {
        struct timespec times[2] = {
            {(time_t)(int64_t)r->old_mode, (long)(r->old_gid >> 32)},
            {(time_t)(int64_t)r->old_uid, (long)(r->old_gid & 0xffffffffu)}
        };
        if (utimensat(h, "original", times, AT_SYMLINK_NOFOLLOW)) { *code = "restore_timestamps"; goto restore; }
        if (move_noreplace(h, "original", parent, name)) { *code = "publish_conflict"; goto done; }
        captured = 0;
    } else if (r->kind == JASH_UNDO_META) {
        int target = openat(h, "original", O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
        if (target < 0) { *code = "open_metadata_target"; goto restore; }
        int ok = !fchown(target, r->old_uid, r->old_gid) && !fchmod(target, r->old_mode);
        close(target);
        if (!ok) { *code = "restore_metadata"; goto restore; }
        if (move_noreplace(h, "original", parent, name)) { *code = "publish_conflict"; goto done; }
        captured = 0;
    } else {
        int target = openat(h, "restored", O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, r->mode & 0777);
        if (target < 0 || lseek(fd, sizeof(*r), SEEK_SET) < 0) { if (target >= 0) close(target); *code = "prepare_restore"; goto restore; }
        unsigned char buf[65536]; uint64_t remaining = r->payload_size; int ok = 1;
        while (remaining) {
            size_t count = remaining > sizeof(buf) ? sizeof(buf) : (size_t)remaining;
            if (read_all(fd, buf, count) || write_all(target, buf, count)) { ok = 0; break; }
            remaining -= count;
        }
        if (ok && fchown(target, r->uid, r->gid)) ok = 0;
        if (ok && fchmod(target, r->mode & 0777)) ok = 0;
        if (ok && fsync(target)) ok = 0;
        if (close(target)) ok = 0;
        if (!ok) { *code = "write_restore"; goto restore; }
        if (move_noreplace(h, "restored", parent, name)) { *code = "publish_conflict"; goto done; }
        /* Only discard the owned postimage after the restored path is durable. */
        if (fsync(parent) || unlinkat(h, "original", 0)) { *code = "restore_cleanup"; goto done; }
        captured = 0;
    }
    if (fsync(h) || fsync(parent)) { *code = "restore_sync"; goto done; }
    r->state = REC_DONE;
    if (store_record(fd, r)) { *code = "consume_sync"; goto done; }
    *code = NULL; result = 0;
    jash_timeline_consume_id(r->id);
    goto done;
restore:
    if (captured && !move_noreplace(h, "original", parent, name)) captured = 0;
done:
    /* On failure retain owned files in the advertised hold. Never recursively
     * delete it: a failed restore may have the only remaining reliable data. */
    if (!result || !*changed) {
        unlinkat(h, "restored", 0);
        unlinkat(h, "restored", AT_REMOVEDIR);
    }
    close(h);
    if (!unlinkat(parent, hold, AT_REMOVEDIR)) hold_path[0] = 0;
    (void)captured;
    return result;
}

static int recovery_store_command(jash_writer_t writer, void* context, const char* args) {
    jash_argv_t av = {0};
    if (jash_argv_build(args, &av) < 0) return 2;
    int gc = 0, dry = 0, pin = -1, mode;
    if (jash_discovery_format(av.argc, av.argv, &mode) < 0) goto invalid;
    uint64_t age = 0, limit = 128;
    const char* only = NULL;
    for (int i = 0; i < av.argc; i++) {
        const char* a = av.argv[i];
        if (jash_discovery_format_flag(a)) {}
        else if (!strcmp(a, "--store")) {}
        else if (!strcmp(a, "--gc")) gc = 1;
        else if (!strcmp(a, "--pin")) pin = 1;
        else if (!strcmp(a, "--unpin")) pin = 0;
        else if (!strcmp(a, "--dry-run")) dry = 1;
        else if (!strcmp(a, "--ref") && !only && i + 1 < av.argc) only = av.argv[++i];
        else if ((!strcmp(a, "--older-than") || !strcmp(a, "--limit")) && i + 1 < av.argc) {
            int is_limit = !strcmp(a, "--limit");
            const char* text = av.argv[++i]; uint64_t n = 0;
            if (!*text) goto invalid;
            for (; *text; text++) {
                if (*text < '0' || *text > '9' || n > (UINT64_MAX - (unsigned)(*text - '0')) / 10) goto invalid;
                n = n * 10 + (unsigned)(*text - '0');
            }
            if (is_limit) { if (!n || n > 4096) goto invalid; limit = n; } else age = n;
        } else goto invalid;
    }
    if (!mode || (only && !token_valid(only)) || (pin >= 0 && (gc || !only))) goto invalid;
    struct sigaction old_pipe, ignore_pipe = {0};
    ignore_pipe.sa_handler = SIG_IGN; sigemptyset(&ignore_pipe.sa_mask);
    if (sigaction(SIGPIPE, &ignore_pipe, &old_pipe)) { jash_argv_free(&av); return 125; }
    jash_budget_unwrap(&writer, &context); jash_redir_stream_writer(writer, context);
    jash_json_writer_t out; jash_jsonw_init(&out, writer, context);
    jash_discovery_rows_t rows = {0};
    const char* operation = gc ? "gc" : pin >= 0 ? pin ? "pin" : "unpin" : "list";
    int rc = 0, truncated = 0;
    uint64_t scanned = 0, affected = 0;
    int store = -1, admission = -1; DIR* directory = NULL;
    if (mode == JASH_DISCOVERY_DATA) {
        if (jash_discovery_rows_init(&rows)) { rc = 125; goto finish; }
        rc = jash_discovery_effects_begin(&rows, writer, context, "undo", mode, "jash.recovery-store.v1");
        if (rc) goto finish;
    } else {
        jash_jsonw_raw(&out, "{\"schema\":\"jash.recovery-store.v1\",\"operation\":");
        jash_jsonw_string(&out, operation);
        jash_jsonw_raw(&out, ",\"consistency\":\"live\",\"items\":[");
        if (jash_jsonw_finish(&out)) { rc = 125; goto finish; }
    }
    store = open_store(0);
    if (store < 0) { if (errno != ENOENT) rc = 1; goto finish; }
    if (pin >= 0) {
        admission = quota_guard(store);
        if (admission < 0) { rc = 1; goto finish; }
    }
    directory = fdopendir(dup(store));
    if (!directory) { rc = 1; goto finish; }
    for (;;) {
        errno = 0; struct dirent* entry = readdir(directory);
        if (!entry) { if (errno) rc = 1; break; }
        const char* token = record_token(entry->d_name);
        if (!token || (only && strcmp(only, token))) continue;
        int pending = !strncmp(entry->d_name, ".pending-", 9);
        int gc_hold = !strncmp(entry->d_name, ".gc-", 4);
        if (scanned == limit) { truncated = 1; break; }
        recovery_record_t r = {0}; struct stat st, named;
        int fd = openat(store, entry->d_name, O_RDWR | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
        const char* action = "invalid";
        int valid = fd >= 0 && !fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_uid == geteuid() &&
                    !(st.st_mode & 0077) && st.st_nlink == 1;
        int pinned = 1, eligible = 0;
        if (valid && flock(fd, LOCK_EX | LOCK_NB)) { action = "busy"; valid = 0; }
        else if (valid) {
            unsigned char digest[32];
            valid = !fstat(fd, &st) && st.st_nlink == 1 && !read_all(fd, &r, sizeof(r)) &&
                    !memcmp(r.magic, REC_MAGIC, 8) && r.record_size == sizeof(r) && coverage(r.kind) && r.state <= REC_DONE &&
                    r.payload_size <= REC_MAX_BYTES && st.st_size == (off_t)(sizeof(r) + r.payload_size) &&
                    !record_digest(fd, &r, digest) && !memcmp(digest, r.record_hash, 32);
        }
        if (valid) {
            pinned = pending || r.state != REC_DONE || (r.reserved & 1u);
            time_t now = time(NULL);
            eligible = !pinned && now >= st.st_mtime && (uint64_t)(now - st.st_mtime) >= age;
            action = "kept";
            if (pending) { action = "orphaned_pending"; rc = 1; }
            else if (pin >= 0) {
                if (dry) action = pin ? "would_pin" : "would_unpin";
                else {
                    r.reserved = (r.reserved & ~1u) | (unsigned)pin;
                    if (store_record(fd, &r)) { action = "write_failed"; rc = 1; }
                    else {
                        action = pin ? "pinned" : "unpinned"; affected++;
                        pinned = r.state != REC_DONE || (r.reserved & 1u);
                        eligible = !pinned && !age;
                    }
                }
            } else if (gc && eligible) {
                action = "would_remove";
                if (!dry) {
                    char hold[40]; memcpy(hold, ".gc-", 4); memcpy(hold + 4, token, 33);
                    if (!gc_hold && move_noreplace(store, entry->d_name, store, hold)) { action = "capture_failed"; rc = 1; }
                    else if (fstatat(store, hold, &named, AT_SYMLINK_NOFOLLOW) || named.st_dev != st.st_dev || named.st_ino != st.st_ino) {
                        if (!gc_hold) (void)move_noreplace(store, hold, store, entry->d_name);
                        action = "identity_changed"; rc = 1;
                    } else if (unlinkat(store, hold, 0)) { action = "remove_failed"; rc = 1; }
                    else {
                        affected++;
                        if (fsync(store)) { action = "removed_sync_failed"; rc = 1; }
                        else action = "removed";
                    }
                }
            }
        } else if (!strcmp(action, "invalid")) rc = 1;
        if (fd >= 0) close(fd);
        const char* state_name = valid ? r.state == REC_READY ? "ready" : r.state == REC_ACTIVE ? "active" : "consumed" : "unknown";
        const char* storage_state = pending ? "pending" : gc_hold ? "gc_hold" : "published";
        if (mode == JASH_DISCOVERY_DATA) {
            scanned++;
            jdv_record_input_t fields[] = {
                {JDV_LITERAL("schema"), jash_discovery_string(&rows, "jash.recovery-store.item.v1")},
                {JDV_LITERAL("operation"), jash_discovery_string(&rows, operation)},
                {JDV_LITERAL("consistency"), jash_discovery_string(&rows, "live")},
                {JDV_LITERAL("ref"), jash_discovery_string(&rows, token)},
                {JDV_LITERAL("storage_state"), jash_discovery_string(&rows, storage_state)},
                {JDV_LITERAL("action"), jash_discovery_string(&rows, action)},
                {JDV_LITERAL("state"), jash_discovery_string(&rows, state_name)},
                {JDV_LITERAL("pinned"), jdv_value_bool(pinned)},
                {JDV_LITERAL("eligible"), jdv_value_bool(eligible)},
                {JDV_LITERAL("payload_bytes"), valid ? jdv_value_filesize(r.payload_size) : jdv_value_nothing()},
                {JDV_LITERAL("scanned"), jdv_value_u64(scanned)},
                {JDV_LITERAL("affected"), jdv_value_u64(affected)}
            };
            if (jash_discovery_add(&rows, fields, sizeof fields / sizeof fields[0])) { rc = 75; break; }
            if (jash_discovery_effects_flush(&rows)) { rc = 125; break; }
        } else {
            if (scanned++) jash_jsonw_raw(&out, ",");
            jash_jsonw_raw(&out, "{\"ref\":"); jash_jsonw_string(&out, token);
            jash_jsonw_raw(&out, ",\"storage_state\":"); jash_jsonw_string(&out, storage_state);
            jash_jsonw_raw(&out, ",\"action\":"); jash_jsonw_string(&out, action);
            jash_jsonw_raw(&out, ",\"state\":"); jash_jsonw_string(&out, state_name);
            jash_jsonw_raw(&out, ",\"pinned\":"); jash_jsonw_raw(&out, pinned ? "true" : "false");
            jash_jsonw_raw(&out, ",\"eligible\":"); jash_jsonw_raw(&out, eligible ? "true" : "false");
            jash_jsonw_raw(&out, ",\"payload_bytes\":");
            if (valid) jash_jsonw_safe_u64(&out, r.payload_size); else jash_jsonw_raw(&out, "null");
            jash_jsonw_raw(&out, "}");
            if (jash_jsonw_finish(&out)) { rc = 125; break; }
        }
    }
    if (only && !scanned) rc = 1;
finish:
    if (directory && closedir(directory) && !rc) rc = 1;
    if (admission >= 0 && close(admission) && !rc) rc = 1;
    if (store >= 0 && close(store) && !rc) rc = 1;
    if (mode == JASH_DISCOVERY_DATA) {
        if (truncated && !rc) rc = 75;
        rc = jash_discovery_effects_end_outcome(&rows, rc,
            truncated && rc == 75 ? JDV_STREAM_INCOMPLETE : rc ? JDV_STREAM_FAILED : JDV_STREAM_COMPLETE,
            truncated, truncated ? "scan_limit" : NULL);
        jash_discovery_rows_free(&rows);
    } else {
        jash_jsonw_raw(&out, "],\"scanned\":"); jash_jsonw_safe_u64(&out, scanned);
        jash_jsonw_raw(&out, ",\"affected\":"); jash_jsonw_safe_u64(&out, affected);
        jash_jsonw_raw(&out, ",\"truncated\":"); jash_jsonw_raw(&out, truncated ? "true" : "false");
        jash_jsonw_raw(&out, ",\"exit_code\":"); jash_jsonw_i64(&out, rc);
        jash_jsonw_raw(&out, "}\n");
        if (jash_jsonw_finish(&out)) rc = 125;
    }
    jash_argv_free(&av);
    if (sigaction(SIGPIPE, &old_pipe, NULL)) rc = 125;
    return rc;
invalid:
    jash_argv_free(&av); return jash_discovery_error(writer, context, "undo", mode, "invalid_recovery_options", 2);
}

static int recovery_quota_command(jash_writer_t writer, void* context, const char* args) {
    jash_argv_t av = {0};
    if (jash_argv_build(args, &av) < 0) return 2;
    int json = 0, set = 0, dry = 0, usage = 0, mode;
    if (jash_discovery_format(av.argc, av.argv, &mode) < 0) goto invalid;
    uint64_t proposed = 0, used = 0, limit = 0;
    for (int i = 0; i < av.argc; i++) {
        if (jash_discovery_format_flag(av.argv[i]) && !json) json = 1;
        else if (!strcmp(av.argv[i], "--usage") && !usage) usage = 1;
        else if (!strcmp(av.argv[i], "--dry-run") && !dry) dry = 1;
        else if (!strcmp(av.argv[i], "--quota") && !set && i + 1 < av.argc) {
            set = 1; if (quota_number(av.argv[++i], &proposed)) goto invalid;
        } else goto invalid;
    }
    if (!json || (!usage && !set)) goto invalid;
    struct sigaction previous, ignore = {0}; ignore.sa_handler = SIG_IGN; sigemptyset(&ignore.sa_mask);
    if (sigaction(SIGPIPE, &ignore, &previous)) { jash_argv_free(&av); return 125; }
    jash_budget_unwrap(&writer, &context); jash_redir_stream_writer(writer, context);
    jash_json_writer_t out; jash_jsonw_init(&out, writer, context);
    jash_discovery_rows_t rows = {0};
    int store = -1, guard = -1, rc = 0, applied = 0, fits = 1, measured = 0;
    const char* error = NULL;
    if (mode == JASH_DISCOVERY_DATA) {
        if (jash_discovery_rows_init(&rows)) { rc = 125; goto done; }
        rc = jash_discovery_effects_begin(&rows, writer, context, "undo", mode, "jash.recovery-quota.v1");
        if (rc) goto done;
    } else {
        jash_jsonw_raw(&out, "{\"schema\":\"jash.recovery-quota.v1\",\"units\":\"logical-record-bytes\"");
        if (jash_jsonw_finish(&out)) { rc = 125; goto done; }
    }
    store = open_store(set && !dry);
    if (store >= 0) {
        guard = quota_guard(store);
        if (guard < 0 || quota_limit(store, &limit) || quota_usage(store, &used)) { error = "quota_measurement_failed"; rc = 1; goto done; }
    } else if (errno != ENOENT) { error = "store_unavailable"; rc = 1; goto done; }
    measured = 1;
    fits = !(set ? proposed : limit) || used <= (set ? proposed : limit);
    if (set && !fits) { error = "quota_below_usage"; rc = 1; }
    else if (set && !dry) {
        char temporary[64], text[32]; snprintf(temporary, sizeof(temporary), ".quota-%ld.tmp", (long)getpid());
        int n = snprintf(text, sizeof(text), "%llu\n", (unsigned long long)proposed);
        int fd = openat(store, temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd < 0) { error = "quota_publication_failed"; rc = 1; goto done; }
        int ok = !fchmod(fd, 0600) && !write_all(fd, text, (size_t)n) && !fsync(fd);
        if (close(fd)) ok = 0;
        if (ok && !renameat(store, temporary, store, ".quota")) { applied = 1; limit = proposed; }
        else ok = 0;
        if (ok && fsync(store)) ok = 0;
        if (!ok) { unlinkat(store, temporary, 0); error = "quota_publication_failed"; rc = 1; }
    }
done:
    if (guard >= 0 && close(guard) && !rc) { error = "quota_close_failed"; rc = 1; }
    if (store >= 0 && close(store) && !rc) { error = "quota_close_failed"; rc = 1; }
    if (mode == JASH_DISCOVERY_DATA) {
        if (rows.effects) {
            jdv_record_input_t fields[] = {
                {JDV_LITERAL("schema"), jash_discovery_string(&rows, "jash.recovery-quota.v1")},
                {JDV_LITERAL("units"), jash_discovery_string(&rows, "logical-record-bytes")},
                {JDV_LITERAL("used_bytes"), measured ? jdv_value_filesize(used) : jdv_value_nothing()},
                {JDV_LITERAL("limit_bytes"), measured ? jdv_value_filesize(limit) : jdv_value_nothing()},
                {JDV_LITERAL("proposed_bytes"), set ? jdv_value_filesize(proposed) : jdv_value_nothing()},
                {JDV_LITERAL("fits"), measured ? jdv_value_bool(fits) : jdv_value_nothing()},
                {JDV_LITERAL("applied"), jdv_value_bool(applied)},
                {JDV_LITERAL("error"), jash_discovery_string(&rows, error)},
                {JDV_LITERAL("exit_code"), jdv_value_u64((uint64_t)rc)}
            };
            (void)jash_discovery_add(&rows, fields, sizeof fields / sizeof fields[0]);
            rc = jash_discovery_effects_end(&rows, rc);
        }
        jash_discovery_rows_free(&rows);
    } else {
        jash_jsonw_raw(&out, ",\"used_bytes\":"); if (measured) jash_jsonw_safe_u64(&out, used); else jash_jsonw_raw(&out, "null");
        jash_jsonw_raw(&out, ",\"limit_bytes\":"); if (measured) jash_jsonw_safe_u64(&out, limit); else jash_jsonw_raw(&out, "null");
        jash_jsonw_raw(&out, ",\"proposed_bytes\":"); if (set) jash_jsonw_safe_u64(&out, proposed); else jash_jsonw_raw(&out, "null");
        jash_jsonw_raw(&out, ",\"fits\":"); jash_jsonw_raw(&out, !measured ? "null" : fits ? "true" : "false");
        jash_jsonw_raw(&out, ",\"applied\":"); jash_jsonw_raw(&out, applied ? "true" : "false");
        jash_jsonw_raw(&out, ",\"error\":"); if (error) jash_jsonw_string(&out, error); else jash_jsonw_raw(&out, "null");
        jash_jsonw_raw(&out, ",\"exit_code\":"); jash_jsonw_i64(&out, rc); jash_jsonw_raw(&out, "}\n");
        if (jash_jsonw_finish(&out)) rc = 125;
    }
    if (sigaction(SIGPIPE, &previous, NULL)) rc = 125;
    jash_argv_free(&av); return rc;
invalid:
    jash_argv_free(&av); return jash_discovery_error(writer, context, "undo", mode, "invalid_recovery_options", 2);
}

int jash_recovery_command(jash_writer_t writer, void* context, const char* args) {
    jash_argv_t parsed = {0};
    const char* token = NULL; int dry = 0, json = 0, valid = 1, inspect = 0, mode;
    if (jash_argv_build(args ? args : "", &parsed) < 0) return 2;
    if (jash_discovery_format(parsed.argc, parsed.argv, &mode) < 0) {
        jash_argv_free(&parsed);
        return jash_discovery_error(writer, context, "undo", mode, "conflicting_formats", 2);
    }
    int quota = 0, maintenance = 0;
    for (int i = 0; i < parsed.argc; i++) {
        const char* arg = parsed.argv[i];
        if (!strcmp(arg, "--")) break;
        if (!strcmp(arg, "--ref")) { i++; continue; }
        if (!strcmp(arg, "--quota") || !strcmp(arg, "--usage")) quota = 1;
        if (!strcmp(arg, "--store") || !strcmp(arg, "--gc") || !strcmp(arg, "--pin") || !strcmp(arg, "--unpin")) maintenance = 1;
    }
    if (quota || maintenance) {
        jash_argv_free(&parsed);
        if (quota && maintenance) return jash_discovery_error(writer, context, "undo", mode, "conflicting_recovery_operations", 2);
        return quota ? recovery_quota_command(writer, context, args) : recovery_store_command(writer, context, args);
    }
    for (int i = 0; i < parsed.argc; i++) {
        const char* arg = parsed.argv[i];
        if (jash_discovery_format_flag(arg) && !json) json = 1;
        else if (!strcmp(arg, "--inspect") && !inspect) inspect = 1;
        else if (!strcmp(arg, "--dry-run") && !dry) dry = 1;
        else if (!strcmp(arg, "--ref") && !token && i + 1 < parsed.argc) token = parsed.argv[++i];
        else valid = 0;
    }
    if (inspect && dry) valid = 0;
    jash_budget_unwrap(&writer, &context); jash_redir_stream_writer(writer, context);
    jash_json_writer_t out; jash_jsonw_init(&out, writer, context);
    struct sigaction old, ignore = {0}; ignore.sa_handler = SIG_IGN; sigemptyset(&ignore.sa_mask);
    if (sigaction(SIGPIPE, &ignore, &old)) { jash_argv_free(&parsed); return 125; }
    jash_discovery_rows_t rows = {0};
    int rc = 1, changed = 0, fd = -1, store = -1, parent = -1;
    const char* code = "invalid_arguments";
    char hold[REC_PATH] = "", name[REC_PATH], absolute[REC_PATH];
    recovery_record_t r = {0};
    if (mode == JASH_DISCOVERY_DATA) {
        if (jash_discovery_rows_init(&rows)) { rc = 125; goto done; }
        rc = jash_discovery_effects_begin(&rows, writer, context, "undo", mode, "jash.undo-result.v1");
        if (rc) goto done;
        rc = 1;
    } else {
        jash_jsonw_raw(&out, "{\"schema\":\"jash.undo-result.v1\",\"ref\":");
        jash_jsonw_string_or_bytes(&out, token);
        if (jash_jsonw_finish(&out)) { rc = 125; goto done; }
    }
    if (!valid || !json || !token_valid(token)) { rc = 2; goto finish; }
    store = open_store(0);
    if (store < 0) { code = "reference_unavailable"; goto finish; }
    fd = openat(store, token, O_RDWR | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) { code = "reference_unavailable"; goto finish; }
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 0077) || st.st_nlink != 1 ||
        flock(fd, LOCK_EX)) { code = "invalid_reference"; goto finish; }
    if (read_all(fd, &r, sizeof(r)) || memcmp(r.magic, REC_MAGIC, 8) || r.record_size != sizeof(r) ||
        !coverage(r.kind) || r.payload_size > REC_MAX_BYTES || st.st_size != (off_t)(sizeof(r) + r.payload_size) ||
        !memchr(r.path, 0, sizeof(r.path)) || r.path[0] != '/' || r.state > REC_DONE) { code = "invalid_reference"; goto finish; }
    unsigned char digest[32];
    if (record_digest(fd, &r, digest) || memcmp(digest, r.record_hash, 32)) { code = "damaged_reference"; goto finish; }
    if (inspect) { rc = 0; code = NULL; goto finish; }
    if (r.state != REC_READY) {
        code = r.state == REC_DONE ? "consumed" : "interrupted_recovery";
        if (r.state == REC_ACTIVE) {
            const char* slash = strrchr(r.path, '/');
            int n = snprintf(hold, sizeof(hold), "%.*s/.jash-undo-%s", (int)(slash - r.path), r.path, token);
            if (n < 0 || (size_t)n >= sizeof(hold)) hold[0] = 0;
        }
        goto finish;
    }
    parent = endpoint(r.path, absolute, name);
    if (parent < 0 || strcmp(r.path, absolute) || fstat(parent, &st) ||
        r.parent_dev != (uint64_t)st.st_dev || r.parent_ino != (uint64_t)st.st_ino) { code = "parent_changed"; goto finish; }
    if (r.kind == JASH_UNDO_DIR_REMOVE) {
        if (!fstatat(parent, name, &st, AT_SYMLINK_NOFOLLOW) || errno != ENOENT) { code = "destination_exists"; goto finish; }
    } else {
        if (inspect_target(parent, name, &r, 0, 0)) { code = "target_changed"; goto finish; }
        if (r.kind == JASH_UNDO_DIR_CREATE && !empty_directory(parent, name)) { code = "directory_not_empty"; goto finish; }
    }
    if (r.kind == JASH_UNDO_MOVE || r.kind == JASH_UNDO_TRASH) {
        char original_name[REC_PATH];
        int original = move_origin(fd, &r, original_name, &code);
        if (original < 0) goto finish;
        close(original);
    }
    if (dry) { rc = 0; code = NULL; }
    else rc = apply_record(fd, &r, parent, name, token, &code, &changed, hold);
finish:
    if (parent >= 0) { if (close(parent) && !rc) { rc = 1; code = "close_failed"; } parent = -1; }
    if (fd >= 0) { if (close(fd) && !rc) { rc = 1; code = "close_failed"; } fd = -1; }
    if (store >= 0) { if (close(store) && !rc) { rc = 1; code = "close_failed"; } store = -1; }
    if (mode == JASH_DISCOVERY_DATA) {
        jdv_value_t record = jdv_value_nothing();
        if (inspect && !rc) {
            char id[32]; snprintf(id, sizeof id, "%llu", (unsigned long long)r.id);
            jdv_record_input_t fields[] = {
                {JDV_LITERAL("entry_id"), jash_discovery_string(&rows, id)},
                {JDV_LITERAL("coverage"), jash_discovery_string(&rows, coverage(r.kind))},
                {JDV_LITERAL("state"), jash_discovery_string(&rows, r.state == REC_READY ? "ready" : r.state == REC_ACTIVE ? "active" : "consumed")},
                {JDV_LITERAL("path"), jash_discovery_path(&rows, r.path)},
                {JDV_LITERAL("payload_bytes"), jdv_value_filesize(r.payload_size)},
                {JDV_LITERAL("target_validated"), jdv_value_bool(0)}
            };
            if (jdv_value_record(&rows.arena, fields, sizeof fields / sizeof fields[0], &record)) rows.error = 1;
        }
        jdv_record_input_t fields[] = {
            {JDV_LITERAL("schema"), jash_discovery_string(&rows, "jash.undo-result.v1")},
            {JDV_LITERAL("ref"), jash_discovery_string(&rows, token)},
            {JDV_LITERAL("status"), jash_discovery_string(&rows, rc ? changed ? "partial" : "failed" : inspect ? "inspected" : dry ? "ready" : "completed")},
            {JDV_LITERAL("effect"), jash_discovery_string(&rows, changed ? "changed" : "none")},
            {JDV_LITERAL("code"), jash_discovery_string(&rows, code)},
            {JDV_LITERAL("exit_code"), jdv_value_u64((uint64_t)rc)},
            {JDV_LITERAL("recovery_path"), jash_discovery_path(&rows, hold[0] ? hold : NULL)},
            {JDV_LITERAL("record"), record}
        };
        (void)jash_discovery_add(&rows, fields, sizeof fields / sizeof fields[0] - (record.kind == JDV_NOTHING));
        rc = jash_discovery_effects_end(&rows, rc);
        goto done;
    }
    jash_jsonw_raw(&out, ",\"status\":");
    jash_jsonw_string(&out, rc ? changed ? "partial" : "failed" : inspect ? "inspected" : dry ? "ready" : "completed");
    if (inspect && !rc) {
        jash_jsonw_raw(&out, ",\"record\":{\"entry_id\":\""); jash_jsonw_u64(&out, r.id);
        jash_jsonw_raw(&out, "\",\"coverage\":"); jash_jsonw_string(&out, coverage(r.kind));
        jash_jsonw_raw(&out, ",\"state\":"); jash_jsonw_string(&out, r.state == REC_READY ? "ready" : r.state == REC_ACTIVE ? "active" : "consumed");
        jash_jsonw_raw(&out, ",\"path\":"); jash_jsonw_string_or_bytes(&out, r.path);
        jash_jsonw_raw(&out, ",\"payload_bytes\":"); jash_jsonw_safe_u64(&out, r.payload_size);
        jash_jsonw_raw(&out, ",\"target_validated\":false}");
    }
    jash_jsonw_raw(&out, ",\"effect\":"); jash_jsonw_string(&out, changed ? "changed" : "none");
    jash_jsonw_raw(&out, ",\"code\":"); if (code) jash_jsonw_string(&out, code); else jash_jsonw_raw(&out, "null");
    jash_jsonw_raw(&out, ",\"exit_code\":"); jash_jsonw_i64(&out, rc);
    jash_jsonw_raw(&out, ",\"recovery_path\":"); jash_jsonw_string_or_bytes(&out, hold[0] ? hold : NULL);
    jash_jsonw_raw(&out, "}\n");
    if (jash_jsonw_finish(&out)) rc = 125;
done:
    if (parent >= 0) close(parent);
    if (fd >= 0) close(fd);
    if (store >= 0) close(store);
    jash_discovery_rows_free(&rows);
    if (sigaction(SIGPIPE, &old, NULL)) rc = 125;
    jash_argv_free(&parsed);
    return rc;
}
