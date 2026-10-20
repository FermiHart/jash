// SPDX-License-Identifier: LicenseRef-SPL-1.0
/* Private backup tests: real files, with read/write/publication fault hooks. */
#include <sys/syscalls.h>
#include <stdio.h>
#include <stdarg.h>
#include <limits.h>
#include <errno.h>

static struct stat source_stat;
static char source_path[PATH_MAX];
static size_t source_bytes, short_read, short_write;
static int stage_fd = -1, interrupt_read, interrupt_write, fail_read, fail_write, fail_close;
static int mutate_on_publish;
static int fake_size;
static int publication_mode;
static char moved_store[PATH_MAX], replacement_stage[PATH_MAX];
static const char* label = "setup";
#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", label, __LINE__, #expr); exit(1); \
} } while (0)

static int is_source(int fd) {
    struct stat st;
    return fstat(fd, &st) == 0 && st.st_dev == source_stat.st_dev && st.st_ino == source_stat.st_ino;
}
static ssize_t test_read(int fd, void* data, size_t len) {
    int source = is_source(fd);
    if (source && interrupt_read) { interrupt_read = 0; errno = EINTR; return -1; }
    if (source && fail_read) { errno = EIO; return -1; }
    if (source && short_read && len > short_read) len = short_read;
    ssize_t n = read(fd, data, len);
    if (source && n > 0) source_bytes += (size_t)n;
    return n;
}
static ssize_t test_write(int fd, const void* data, size_t len) {
    if (fd == stage_fd) {
        if (interrupt_write) { interrupt_write = 0; errno = EINTR; return -1; }
        if (fail_write) { errno = ENOSPC; return -1; }
        if (short_write && len > short_write) len = short_write;
    }
    return write(fd, data, len);
}
static int test_fstat(int fd, struct stat* st) {
    int rc = fstat(fd, st);
    if (rc == 0 && fake_size && st->st_dev == source_stat.st_dev && st->st_ino == source_stat.st_ino)
        st->st_size = fake_size == 1 ? 128 : 65537;
    return rc;
}
static void stage_opened(int fd, int flags, const char* path) {
    if (fd >= 0 && (flags & O_WRONLY) && strstr(path, ".tmp.")) {
        stage_fd = fd;
        if (mutate_on_publish) {
            int target = open(source_path, O_WRONLY | O_TRUNC);
            CHECK(target >= 0);
            CHECK(write(target, "CHANGED", 7) == 7);
            CHECK(close(target) == 0);
            mutate_on_publish = 0;
        }
    }
}
static int test_open(const char* path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }
    int fd = open(path, flags, mode);
    stage_opened(fd, flags, path);
    return fd;
}
static int test_openat(int dirfd, const char* path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }
    if (publication_mode == 18 && (flags & O_CREAT)) { errno = EMFILE; return -1; }
    if (publication_mode == 16 && (flags & O_CREAT)) {
        publication_mode = 0;
        int other = openat(dirfd, path, O_WRONLY | O_CREAT | O_EXCL, 0600);
        CHECK(other >= 0 && write(other, "KEEP", 4) == 4 && close(other) == 0);
    }
    int fd = openat(dirfd, path, flags, mode);
    stage_opened(fd, flags, path);
    return fd;
}
static int test_mkdir(const char* path, mode_t mode) {
    const char* last = strrchr(path, '/');
    if (publication_mode == 11 && last && !strcmp(last, "/shared")) {
        errno = EACCES;
        return -1;
    }
    return mkdir(path, mode);
}
static int before_publish(const char* old, const char* destination) {
    if (publication_mode == 10) { errno = EIO; return -1; }
    if (publication_mode == 12 || publication_mode == 13) {
        int replacement = publication_mode == 12;
        publication_mode = 0;
        char directory[PATH_MAX];
        const char* slash = strrchr(destination, '/');
        CHECK(slash && (size_t)(slash - destination) < sizeof directory);
        memcpy(directory, destination, (size_t)(slash - destination));
        directory[slash - destination] = 0;
        CHECK(snprintf(moved_store, sizeof moved_store, "%s-moved", directory) < (int)sizeof moved_store);
        CHECK(rename(directory, moved_store) == 0);
        const char* name = strrchr(old, '/');
        name = name ? name + 1 : old;
        CHECK(snprintf(replacement_stage, sizeof replacement_stage, "%s/%s", directory, name) < (int)sizeof replacement_stage);
        if (replacement) {
            CHECK(mkdir(directory, 0700) == 0);
            int fd = open(replacement_stage, O_WRONLY | O_CREAT | O_EXCL, 0600);
            CHECK(fd >= 0 && write(fd, "KEEP", 4) == 4 && close(fd) == 0);
        }
    }
    return 0;
}
static int test_rename(const char* old, const char* destination) {
    if (before_publish(old, destination) != 0) return -1;
    return rename(old, destination);
}
static int test_renameat(int oldfd, const char* old, int newfd, const char* destination) {
    if (publication_mode == 19) {
        publication_mode = 0;
        CHECK(mkfifo(destination, 0600) == 0);
        errno = EIO;
        return -1;
    }
    if (publication_mode == 14 || publication_mode == 15) {
        int valid = publication_mode == 14;
        publication_mode = 0;
        if (valid) CHECK(linkat(oldfd, old, newfd, destination, 0) == 0);
        else {
            int fd = open(destination, O_WRONLY | O_CREAT | O_EXCL, 0600);
            CHECK(fd >= 0);
            for (off_t i = 0; i < source_stat.st_size; i++) CHECK(write(fd, "!", 1) == 1);
            CHECK(close(fd) == 0);
        }
        errno = EIO;
        return -1;
    }
    if (before_publish(old, destination) != 0) return -1;
    return renameat(oldfd, old, newfd, destination);
}
static int test_close(int fd) {
    int stage = fd == stage_fd;
    if (stage) stage_fd = -1;
    int rc = close(fd);
    if (stage && fail_close) { errno = EIO; return -1; }
    return rc;
}

#define JASH_AGENT_ONLY
#define read test_read
#define write test_write
#define open test_open
#define close test_close
#define fstat test_fstat
#define openat test_openat
#define rename test_rename
#define renameat test_renameat
#define mkdir test_mkdir
#include "core/timeline.c"
#undef read
#undef write
#undef open
#undef close
#undef fstat
#undef openat
#undef rename
#undef renameat
#undef mkdir

int jash_sigil_get(void) { return 0; }
int osif_rmtree(const char* path) { (void)path; return 0; }

static unsigned cases;
static int descriptor_count(void) {
    int count = 0;
    for (int fd = 0; fd < 256; fd++) if (fcntl(fd, F_GETFD) >= 0) count++;
    return count;
}
static void run_case(const char* root, const char* name, size_t size, int mode) {
    label = name;
    char dir[PATH_MAX];
    int n = snprintf(dir, sizeof dir, "%s/case-%u", root, cases++);
    CHECK(n > 0 && (size_t)n < sizeof dir);
    CHECK(mkdir(dir, 0700) == 0);
    CHECK(setenv("TMPDIR", dir, 1) == 0);
    memset(g_backup_dir, 0, sizeof g_backup_dir);
    n = snprintf(source_path, sizeof source_path, "%s/input", dir);
    CHECK(n > 0 && (size_t)n < sizeof source_path);
    unsigned char* seed = malloc(size ? size : 1);
    CHECK(seed);
    for (size_t i = 0; i < size; i++) seed[i] = (unsigned char)(i * 19 + cases);
    int fd = open(source_path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    CHECK(fd >= 0);
    size_t written = 0;
    while (written < size) {
        ssize_t part = write(fd, seed + written, size - written);
        CHECK(part > 0);
        written += (size_t)part;
    }
    CHECK(close(fd) == 0 && stat(source_path, &source_stat) == 0);
    if (mode == 17) {
        char store[PATH_MAX];
        CHECK(snprintf(store, sizeof store, "%s/shared", dir) < (int)sizeof store);
        int other = open(store, O_WRONLY | O_CREAT | O_EXCL, 0600);
        CHECK(other >= 0 && write(other, "KEEP", 4) == 4 && close(other) == 0);
    }
    source_bytes = 0;
    stage_fd = -1;
    short_read = mode == 1 ? 7 : 0;
    short_write = mode == 1 ? 3 : 0;
    interrupt_read = mode == 2;
    interrupt_write = mode == 3;
    fail_read = mode == 4;
    fail_write = mode == 5;
    fail_close = mode == 6;
    mutate_on_publish = mode == 7;
    fake_size = mode == 8 ? 1 : mode == 9 ? 2 : 0;
    publication_mode = mode;
    moved_store[0] = replacement_stage[0] = 0;

    char backup[1024];
    unsigned char expected_digest[32];
    char expected_hex[65], expected_path[PATH_MAX], expected_stage[PATH_MAX];
    jash_sha256(seed, size, expected_digest);
    jash_sha256_hex(expected_digest, expected_hex);
    CHECK(snprintf(expected_path, sizeof expected_path, "%s/shared/bk_%s", dir, expected_hex) < (int)sizeof expected_path);
    CHECK(snprintf(expected_stage, sizeof expected_stage, "%s.tmp.%ld", expected_path, (long)getpid()) < (int)sizeof expected_stage);
    int descriptors = descriptor_count();
    /* A FIFO with no writer must be rejected without blocking in open(). */
    if (mode == 19) alarm(3);
    int rc = jash_timeline_backup_file(source_path, backup, sizeof backup);
    if (mode == 19) alarm(0);
    CHECK(descriptor_count() == descriptors);
    if ((mode >= 4 && mode <= 6) || mode == 10 || mode == 11 || mode == 13 || mode >= 15) {
        CHECK(rc != 0);
        CHECK(backup[0] == '\0');
        CHECK(stage_fd == -1);
        if (mode != 4) {
            if (mode != 15 && mode != 19) CHECK(access(expected_path, F_OK) != 0);
            if (mode != 16) CHECK(access(expected_stage, F_OK) != 0);
        }
    } else {
        CHECK(rc == 0 && stage_fd == -1);
        unsigned char digest[32];
        char hex[65];
        jash_sha256(seed, size, digest);
        jash_sha256_hex(digest, hex);
        const char* base = strrchr(backup, '/');
        CHECK(base && !strncmp(base + 1, "bk_", 3) && !strcmp(base + 4, hex));
        fd = open(backup, O_RDONLY);
        CHECK(fd >= 0);
        unsigned char* got = malloc(size + 1);
        CHECK(got);
        size_t used = 0;
        ssize_t part;
        while ((part = read(fd, got + used, size + 1 - used)) > 0) used += (size_t)part;
        CHECK(part == 0 && used == size && !memcmp(got, seed, size));
        CHECK(close(fd) == 0);
        free(got);
        if (size <= 65536 && mode != 9) CHECK(source_bytes == size); /* no source reread */
        if (mode != 7) {
            struct stat first, again;
            CHECK(stat(backup, &first) == 0);
            char second[1024];
            CHECK(jash_timeline_backup_file(source_path, second, sizeof second) == 0);
            CHECK(!strcmp(backup, second) && stat(second, &again) == 0);
            CHECK(first.st_ino == again.st_ino && first.st_dev == again.st_dev);
        }
    }
    if (mode == 12) {
        char kept[5] = {0};
        fd = open(replacement_stage, O_RDONLY);
        CHECK(fd >= 0 && read(fd, kept, 4) == 4 && close(fd) == 0);
        CHECK(!strcmp(kept, "KEEP"));
    }
    if (mode == 16) {
        char kept[5] = {0};
        fd = open(expected_stage, O_RDONLY);
        CHECK(fd >= 0 && read(fd, kept, 4) == 4 && close(fd) == 0);
        CHECK(!strcmp(kept, "KEEP"));
    }
    if (mode == 19) {
        struct stat kept;
        CHECK(lstat(expected_path, &kept) == 0 && S_ISFIFO(kept.st_mode));
    }
    if (mode == 12 || mode == 13) {
        const char* name = strrchr(replacement_stage, '/');
        char moved_temporary[PATH_MAX];
        CHECK(name && snprintf(moved_temporary, sizeof moved_temporary, "%s%s", moved_store, name) < (int)sizeof moved_temporary);
        CHECK(access(moved_temporary, F_OK) != 0);
    }
    free(seed);
    printf("PASS %s\n", name);
}

int main(int argc, char** argv) {
    CHECK(argc == 2 || argc == 3);
    if (argc == 3) {
        if (!strcmp(argv[2], "fifo"))
            run_case(argv[1], "FIFO publication is rejected without blocking", 128, 19);
        else {
            CHECK(!strcmp(argv[2], "namespace"));
            run_case(argv[1], "namespace replacement preserves both files", 128, 12);
        }
        return 0;
    }
    run_case(argv[1], "empty", 0, 0);
    run_case(argv[1], "single byte", 1, 0);
    run_case(argv[1], "binary small", 4096, 0);
    run_case(argv[1], "capture lower boundary", 65535, 0);
    run_case(argv[1], "capture full boundary", 65536, 0);
    run_case(argv[1], "streaming boundary", 65537, 0);
    run_case(argv[1], "streaming large", 131072, 0);
    run_case(argv[1], "short reads and writes", 123, 1);
    run_case(argv[1], "read EINTR", 128, 2);
    run_case(argv[1], "write EINTR", 128, 3);
    run_case(argv[1], "read failure", 128, 4);
    run_case(argv[1], "write failure removes stage", 128, 5);
    run_case(argv[1], "close failure removes stage", 128, 6);
    run_case(argv[1], "copy the same bytes that were hashed", 128, 7);
    run_case(argv[1], "growth past capture threshold", 65537, 8);
    run_case(argv[1], "shrink below streaming size", 1024, 9);
    run_case(argv[1], "publication failure removes stage", 128, 10);
    run_case(argv[1], "store creation failure closes descriptors", 128, 11);
    run_case(argv[1], "namespace replacement preserves both files", 128, 12);
    run_case(argv[1], "renamed store without replacement fails cleanly", 128, 13);
    run_case(argv[1], "verified concurrent publication is reused", 128, 14);
    run_case(argv[1], "same-sized corrupt publication is rejected", 128, 15);
    run_case(argv[1], "preexisting temporary is not overwritten", 128, 16);
    run_case(argv[1], "non-directory store is refused", 128, 17);
    run_case(argv[1], "streaming descriptor exhaustion closes all handles", 131072, 18);
    run_case(argv[1], "FIFO publication is rejected without blocking", 128, 19);
    label = "source symlink refusal and short output buffer";
    char link_path[PATH_MAX];
    CHECK(snprintf(link_path, sizeof link_path, "%s/link", argv[1]) > 0);
    CHECK(symlink(source_path, link_path) == 0);
    char backup[1024];
    int descriptors = descriptor_count();
    size_t before = source_bytes;
    CHECK(jash_timeline_backup_file(link_path, backup, sizeof backup) != 0);
    CHECK(source_bytes == before && descriptor_count() == descriptors);
    char tiny[8];
    memset(tiny, 0x7f, sizeof tiny);
    CHECK(jash_timeline_backup_file(source_path, tiny, 5) != 0);
    CHECK(tiny[0] == '\0' && tiny[5] == 0x7f && descriptor_count() == descriptors);
    cases++;
    puts("PASS source symlink refusal and short output buffer");
    label = "canonical basename uses current physical directory";
    int cwd = open(".", O_RDONLY);
    CHECK(cwd >= 0);
    char physical[PATH_MAX], via_link[PATH_MAX], resolved[PATH_MAX], expected[PATH_MAX];
    snprintf(physical, sizeof physical, "%s/physical", argv[1]);
    snprintf(via_link, sizeof via_link, "%s/via-link", argv[1]);
    CHECK(mkdir(physical, 0700) == 0 && symlink(physical, via_link) == 0);
    CHECK(chdir(via_link) == 0);
    CHECK(getcwd(expected, sizeof expected));
    size_t prefix = strlen(expected);
    CHECK(symlink("elsewhere", "name") == 0);
    CHECK(prefix + sizeof("/name") <= sizeof expected);
    memcpy(expected + prefix, "/name", sizeof("/name"));
    tl_canon("name", resolved, sizeof resolved);
    CHECK(!strcmp(resolved, expected));
    expected[prefix] = '\0';
    tl_canon(".", resolved, sizeof resolved);
    CHECK(!strcmp(resolved, expected));
    CHECK(chdir("/") == 0);
    tl_canon("name", resolved, sizeof resolved);
    CHECK(!strcmp(resolved, "/name"));
    CHECK(fchdir(cwd) == 0 && close(cwd) == 0);
    cases++;
    puts("PASS canonical basename uses current physical directory");
    label = "per-pid directory is created only on explicit request";
    CHECK(access(g_backup_dir, F_OK) != 0);
    const char* private_dir = jash_timeline_backup_dir();
    struct stat private_stat;
    CHECK(stat(private_dir, &private_stat) == 0 && S_ISDIR(private_stat.st_mode));
    CHECK(jash_timeline_backup_dir() == private_dir);
    cases++;
    puts("PASS per-pid directory is created only on explicit request");
    printf("append backup unit: %u cases passed\n", cases);
    return 0;
}
