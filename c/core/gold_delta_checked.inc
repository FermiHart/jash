// SPDX-License-Identifier: LicenseRef-SPL-1.0
/* Checked observation of the existing gold delta store. Encoding and store
 * ownership live here; builtin callers only choose semantic entries. */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/file.h>
#include <unistd.h>
#include <module/blake3.h>
#include <osif/osif_stat.h>

#define GOLD_CHECKED_BYTES (2u * 1024u * 1024u)
#define GOLD_CHECKED_ITEMS 4096u
static const unsigned char GOLD_MAGIC[8] = {'#','J','G','O','L','D','2',0};

static int gold_checked_fail(int error) { errno = error; return -1; }
static int gold_component(const char* value) {
    if (!value || !*value || strlen(value) > 63u) return 0;
    for (const unsigned char* p = (const unsigned char*)value; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '-' || *p == '_')) return 0;
    return 1;
}
static int gold_trusted(int fd, int directory) {
    struct stat st;
    if (fstat(fd, &st)) return -1;
    if ((directory ? !S_ISDIR(st.st_mode) : !S_ISREG(st.st_mode)) ||
        st.st_uid != geteuid() || (st.st_mode & 0022) || (!directory && st.st_nlink != 1))
        return gold_checked_fail(EPERM);
    return 0;
}
static int gold_child(int parent, const char* name, int create, int* created) {
    int fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0 && errno == ENOENT && create) {
        if (mkdirat(parent, name, 0700)) {
            if (errno != EEXIST) return -1;
        } else {
            (*created)++;
            if (fsync(parent)) return -1;
        }
        fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    }
    if (fd < 0) return -1;
    if (gold_trusted(fd, 1)) { int saved = errno; (void)close(fd); return gold_checked_fail(saved); }
    return fd;
}
static int gold_directory(const char* builtin, int create, int* created) {
    const char* home = getenv("HOME");
    if (!home || !*home) return gold_checked_fail(ENOENT);
    int fd = open(home, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return -1;
    const char* names[] = {".jash", "gold-delta", builtin};
    for (size_t i = 0; i < 3; i++) {
        int child = gold_child(fd, names[i], create, created);
        int saved = errno;
        if (close(fd) && child >= 0) { saved = errno; (void)close(child); child = -1; }
        if (child < 0) return gold_checked_fail(saved);
        fd = child;
    }
    return fd;
}
static void gold_digest(const void* bytes, size_t size, unsigned char digest[32]) {
    struct blake3 hash; blake3_init(&hash); blake3_update(&hash, bytes, size); blake3_final(&hash, digest, 32);
}
static int gold_read_snapshot(int directory, const char* name, unsigned char** bytes,
                               size_t* size, int* present) {
    *bytes = NULL; *size = 0; *present = 0;
    int fd = openat(directory, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return errno == ENOENT ? 0 : -1;
    *present = 1;
    struct stat before, after;
    int error = 0;
    if (gold_trusted(fd, 0) || fstat(fd, &before)) error = errno;
    else if (before.st_size < 0 || (uint64_t)before.st_size > GOLD_CHECKED_BYTES) error = E2BIG;
    if (!error) {
        *size = (size_t)before.st_size;
        *bytes = malloc(*size + 1u);
        if (!*bytes) error = ENOMEM;
    }
    size_t used = 0;
    while (!error && used < *size) {
        ssize_t got = read(fd, *bytes + used, *size - used);
        if (got < 0) { if (errno == EINTR) continue; error = errno; }
        else if (!got) error = ESTALE;
        else used += (size_t)got;
    }
    if (!error) {
        unsigned char extra; ssize_t got;
        do { got = read(fd, &extra, 1); } while (got < 0 && errno == EINTR);
        if (got < 0) error = errno;
        else if (got) error = ESTALE;
        else if (fstat(fd, &after)) error = errno;
        else if (before.st_dev != after.st_dev || before.st_ino != after.st_ino ||
            before.st_size != after.st_size ||
            BEAR_ST_MTIM(before).tv_sec != BEAR_ST_MTIM(after).tv_sec ||
            BEAR_ST_MTIM(before).tv_nsec != BEAR_ST_MTIM(after).tv_nsec ||
            BEAR_ST_CTIM(before).tv_sec != BEAR_ST_CTIM(after).tv_sec ||
            BEAR_ST_CTIM(before).tv_nsec != BEAR_ST_CTIM(after).tv_nsec) error = ESTALE;
    }
    if (close(fd) && !error) error = errno;
    if (error) { free(*bytes); *bytes = NULL; return gold_checked_fail(error); }
    (*bytes)[*size] = 0;
    return 0;
}
static uint64_t gold_get_u64(const unsigned char* bytes) {
    uint64_t value = 0; for (unsigned i = 0; i < 8; i++) value = (value << 8) | bytes[i]; return value;
}
static void gold_put_u64(unsigned char* bytes, uint64_t value) {
    for (unsigned i = 0; i < 8; i++) bytes[7u-i] = (unsigned char)(value >> (8u*i));
}
static int64_t gold_signed(uint64_t value) {
    return value <= INT64_MAX ? (int64_t)value : -1 - (int64_t)(UINT64_MAX - value);
}
static int gold_entry_order(const jash_gold_dent_t* entries, size_t count) {
    if (count > GOLD_CHECKED_ITEMS) return gold_checked_fail(E2BIG);
    for (size_t i = 0; i < count; i++) {
        size_t length = strnlen(entries[i].name, sizeof(entries[i].name));
        if (!length || length == sizeof(entries[i].name) || strchr(entries[i].name, '/') ||
            !strcmp(entries[i].name, ".") || !strcmp(entries[i].name, "..") || entries[i].size < 0 ||
            (i && strcmp(entries[i-1].name, entries[i].name) >= 0)) return gold_checked_fail(EINVAL);
    }
    return 0;
}
static int gold_legacy_integer(const char* text, int nonnegative, int64_t* value) {
    if (!text || !*text || *text == '+' || (*text == '-' && nonnegative)) return -1;
    const char* start = *text == '-' ? text + 1 : text;
    if (!*start) return -1;
    for (const char* p = start; *p; p++) if (*p < '0' || *p > '9') return -1;
    errno = 0; char* end; long long number = strtoll(text, &end, 10);
    if (errno || *end) return -1;
    *value = (int64_t)number; return 0;
}
static int gold_decode_snapshot(unsigned char* data, size_t size,
        jash_gold_dent_t* entries, size_t max, size_t* count, int64_t* timestamp) {
    *count = 0; *timestamp = 0;
    if (size >= sizeof(GOLD_MAGIC) && !memcmp(data, GOLD_MAGIC, sizeof(GOLD_MAGIC))) {
        if (size < 24u) return gold_checked_fail(EINVAL);
        uint64_t observed = gold_get_u64(data + 16);
        if (observed > max || observed > GOLD_CHECKED_ITEMS) return gold_checked_fail(E2BIG);
        uint64_t when = gold_get_u64(data + 8);
        if (when > INT64_MAX) return gold_checked_fail(EINVAL);
        *timestamp = (int64_t)when;
        size_t at = 24u;
        for (uint64_t i = 0; i < observed; i++) {
            if (size - at < 2u) return gold_checked_fail(EINVAL);
            size_t length = ((size_t)data[at] << 8) | data[at+1u]; at += 2;
            if (!length || length >= sizeof(entries[0].name) || length > size - at || size - at - length < 17u ||
                memchr(data+at, 0, length)) return gold_checked_fail(EINVAL);
            jash_gold_dent_t* entry = &entries[(*count)++]; memset(entry, 0, sizeof(*entry));
            memcpy(entry->name, data+at, length); at += length;
            entry->fp = (long long)gold_signed(gold_get_u64(data+at)); at += 8;
            uint64_t bytes = gold_get_u64(data+at); at += 8;
            if (bytes > INT64_MAX) return gold_checked_fail(EINVAL);
            entry->size = (long long)bytes; entry->tag = (char)data[at++];
        }
        if (at != size) return gold_checked_fail(EINVAL);
    } else {
        if (!size || memchr(data, 0, size) || data[size-1u] != '\n' || memchr(data, '\r', size) ||
            size < 10u || memcmp(data, "#JGOLD1 ", 8)) return gold_checked_fail(EINVAL);
        char* cursor = (char*)data; char* newline = strchr(cursor, '\n');
        *newline = 0;
        if (gold_legacy_integer(cursor + 8, 1, timestamp)) return gold_checked_fail(EINVAL);
        cursor = newline + 1;
        while (*cursor) {
            if (*count == max || *count == GOLD_CHECKED_ITEMS) return gold_checked_fail(E2BIG);
            newline = strchr(cursor, '\n'); if (!newline) return gold_checked_fail(EINVAL);
            *newline = 0;
            char* first = strchr(cursor, '\t');
            char* second = first ? strchr(first+1, '\t') : NULL;
            char* third = second ? strchr(second+1, '\t') : NULL;
            if (!first || !second || !third || !third[1] || third[2] || (size_t)(first-cursor) >= sizeof(entries[0].name)) return gold_checked_fail(EINVAL);
            *first = *second = *third = 0;
            int64_t fingerprint, bytes;
            if (gold_legacy_integer(first+1, 0, &fingerprint) || gold_legacy_integer(second+1, 1, &bytes)) return gold_checked_fail(EINVAL);
            jash_gold_dent_t* entry = &entries[(*count)++]; memset(entry, 0, sizeof(*entry));
            memcpy(entry->name, cursor, (size_t)(first-cursor)); entry->fp = (long long)fingerprint;
            entry->size = (long long)bytes; entry->tag = third[1]; cursor = newline+1;
        }
    }
    return gold_entry_order(entries, *count);
}

int jash_gold_delta_load_checked(const char* builtin, const char* key,
                          jash_gold_dent_t* prev, size_t max,
                          jash_gold_delta_observation_t* observation) {
    if (!observation) return gold_checked_fail(EINVAL);
    memset(observation, 0, sizeof(*observation));
    if (!gold_component(builtin) || !gold_component(key) || (!prev && max) || max > GOLD_CHECKED_ITEMS) {
        observation->error_number = EINVAL; return gold_checked_fail(EINVAL);
    }
    const char* home = getenv("HOME");
    int written = snprintf(observation->path, sizeof(observation->path), "%s/.jash/gold-delta/%s/%s.snap", home ? home : "", builtin, key);
    if (!home || !*home || written < 0 || (size_t)written >= sizeof(observation->path)) {
        observation->error_number = E2BIG; return gold_checked_fail(E2BIG);
    }
    char name[72]; (void)snprintf(name, sizeof(name), "%s.snap", key);
    int created = 0, directory = gold_directory(builtin, 0, &created);
    if (directory < 0) {
        if (errno == ENOENT) { observation->complete = 1; return 0; }
        observation->error_number = errno; return -1;
    }
    unsigned char* bytes = NULL;
    int rc = gold_read_snapshot(directory, name, &bytes, &observation->bytes, &observation->present);
    int error = rc ? errno : 0;
    if (close(directory) && !error) error = errno;
    if (!error && observation->present) {
        gold_digest(bytes, observation->bytes, observation->digest);
        if (gold_decode_snapshot(bytes, observation->bytes, prev, max, &observation->count, &observation->timestamp_seconds)) error = errno;
    }
    free(bytes);
    if (error) { observation->error_number = error; observation->count = 0; return gold_checked_fail(error); }
    time_t now = time(NULL);
    if (now == (time_t)-1) { observation->error_number = errno ? errno : EIO; return gold_checked_fail(observation->error_number); }
    observation->age_seconds = (int64_t)now >= observation->timestamp_seconds ? (int64_t)now - observation->timestamp_seconds : 0;
    observation->complete = 1; return 0;
}

int jash_gold_delta_store_checked(const char* builtin, const char* key,
                          const jash_gold_dent_t* cur, size_t count,
                          const jash_gold_delta_observation_t* expected,
                          int (*poll)(void*), void* poll_context,
                          jash_gold_delta_publication_t* publication) {
    if (!publication) return gold_checked_fail(EINVAL);
    memset(publication, 0, sizeof(*publication)); publication->phase = "validation";
    if (!gold_component(builtin) || !gold_component(key) || (!cur && count) || !expected || !expected->complete) {
        publication->error_number = EINVAL; return gold_checked_fail(EINVAL);
    }
    const char* home = getenv("HOME"); char target[4096];
    int path_length = snprintf(target, sizeof(target), "%s/.jash/gold-delta/%s/%s.snap", home ? home : "", builtin, key);
    if (!home || !*home || path_length < 0 || (size_t)path_length >= sizeof(target) || strcmp(target, expected->path)) {
        publication->error_number = ESTALE; return gold_checked_fail(ESTALE);
    }
    if (gold_entry_order(cur, count)) { publication->error_number = errno; return -1; }
    size_t total = 24u;
    for (size_t i = 0; i < count; i++) total += 19u + strlen(cur[i].name);
    if (total > GOLD_CHECKED_BYTES) { publication->error_number = E2BIG; return gold_checked_fail(E2BIG); }
    time_t now = time(NULL);
    if (now < 0) { publication->error_number = EIO; return gold_checked_fail(EIO); }
    unsigned char* encoded = malloc(total);
    if (!encoded) { publication->error_number = ENOMEM; return -1; }
    memcpy(encoded, GOLD_MAGIC, 8); gold_put_u64(encoded+8, (uint64_t)now); gold_put_u64(encoded+16, count);
    size_t at = 24u;
    for (size_t i = 0; i < count; i++) {
        size_t length = strlen(cur[i].name); encoded[at++] = (unsigned char)(length >> 8); encoded[at++] = (unsigned char)length;
        memcpy(encoded+at, cur[i].name, length); at += length;
        gold_put_u64(encoded+at, (uint64_t)cur[i].fp); at += 8;
        gold_put_u64(encoded+at, (uint64_t)cur[i].size); at += 8; encoded[at++] = (unsigned char)cur[i].tag;
    }
    int directory = -1, lock = -1, fd = -1, error = 0, temporary_present = 0;
    unsigned char* old = NULL; size_t old_size = 0; int old_present = 0;
    char name[72], temporary[128]; temporary[0] = 0;
    (void)snprintf(name, sizeof(name), "%s.snap", key);
    if (poll && poll(poll_context)) { error = ECANCELED; goto done; }
    publication->phase = "directories";
    directory = gold_directory(builtin, 1, &publication->directories_created);
    if (directory < 0) { error = errno; goto done; }
    publication->phase = "lock";
    lock = openat(directory, ".delta-lock", O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (lock >= 0) publication->lock_created = 1;
    else if (errno == EEXIST) lock = openat(directory, ".delta-lock", O_RDWR | O_NOFOLLOW | O_CLOEXEC);
    if (lock < 0 || gold_trusted(lock, 0)) { error = errno; goto done; }
    while (flock(lock, LOCK_EX | LOCK_NB)) {
        if (errno != EWOULDBLOCK && errno != EAGAIN && errno != EINTR) { error = errno; goto done; }
        if (poll && poll(poll_context)) { error = ECANCELED; goto done; }
        struct timespec delay = {0, 5000000}; (void)nanosleep(&delay, NULL);
    }
    publication->phase = "compare";
    if (gold_read_snapshot(directory, name, &old, &old_size, &old_present)) { error = errno; goto done; }
    unsigned char observed[32] = {0}; if (old_present) gold_digest(old, old_size, observed);
    if (old_present != expected->present || (old_present && (old_size != expected->bytes || memcmp(observed, expected->digest, 32)))) { error = ESTALE; goto done; }
    publication->phase = "temporary";
    static unsigned long sequence;
    for (unsigned retry = 0; retry < 64; retry++) {
        (void)snprintf(temporary, sizeof(temporary), ".%s-%ld-%lu.tmp", key, (long)getpid(), ++sequence);
        fd = openat(directory, temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd >= 0) { temporary_present = 1; break; }
        if (errno != EEXIST) { error = errno; goto done; }
    }
    if (fd < 0) { error = EEXIST; goto done; }
    publication->phase = "write";
    size_t used = 0;
    while (used < total) {
        ssize_t written = write(fd, encoded+used, total-used);
        if (written < 0) { if (errno == EINTR) continue; error = errno; goto done; }
        if (!written) { error = EIO; goto done; }
        used += (size_t)written;
    }
    publication->bytes_written = used;
    publication->phase = "file_sync";
    if (fsync(fd)) { error = errno; goto done; }
    struct stat opened, named;
    if (fstat(fd, &opened) || fstatat(directory, temporary, &named, AT_SYMLINK_NOFOLLOW)) { error = errno; goto done; }
    if (opened.st_dev != named.st_dev || opened.st_ino != named.st_ino || !S_ISREG(named.st_mode) || named.st_size != (off_t)total) { error = ESTALE; goto done; }
    if (poll && poll(poll_context)) { error = ECANCELED; goto done; }
    publication->phase = "publish";
    if (renameat(directory, temporary, directory, name)) { error = errno; goto done; }
    temporary_present = 0; publication->applied = 1;
    if (fstatat(directory, name, &named, AT_SYMLINK_NOFOLLOW)) { error = errno; goto done; }
    if (opened.st_dev != named.st_dev || opened.st_ino != named.st_ino) { error = ESTALE; goto done; }
    publication->phase = "directory_sync";
    if (fsync(directory)) { error = errno; goto done; }
    publication->durable = 1; publication->phase = "complete";
done:
    free(encoded); free(old);
    if (fd >= 0 && close(fd) && !error) { error = errno; publication->phase = "close_file"; }
    if (temporary_present && unlinkat(directory, temporary, 0) && !error) error = errno;
    if (lock >= 0 && close(lock) && !error) { error = errno; publication->phase = "close_lock"; }
    if (directory >= 0 && close(directory) && !error) { error = errno; publication->phase = "close_directory"; }
    if (error) { publication->error_number = error; return gold_checked_fail(error); }
    return 0;
}
