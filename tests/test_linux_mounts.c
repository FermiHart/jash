// SPDX-License-Identifier: LicenseRef-SPL-1.0
/* Linux OSIF mount enumeration: long rows, exact escaped paths and failures.
 * Exercise the public provider with deterministic input; no mount privileges
 * or assumptions about the host's overlay layout are needed. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <sys/syscalls.h>

static FILE* mount_fixture;
static int proc_missing;
static unsigned lookups;
static char last_lookup[256];

static FILE* fixture_fopen(const char* path, const char* mode) {
    assert(!strcmp(mode, "r"));
    assert(!strcmp(path, "/proc/mounts") || !strcmp(path, "/etc/mtab"));
    if (proc_missing && !strcmp(path, "/proc/mounts")) {
        errno = ENOENT;
        return NULL;
    }
    FILE* stream = mount_fixture;
    mount_fixture = NULL;
    assert(stream);
    return stream;
}

static int fixture_statvfs(const char* path, struct statvfs* info) {
    assert(strlen(path) < sizeof(last_lookup));
    memcpy(last_lookup, path, strlen(path) + 1);
    lookups++;
    memset(info, 0, sizeof(*info));
    info->f_frsize = 4096;
    info->f_blocks = 100;
    info->f_bfree = 25;
    info->f_bavail = 20;
    if (!strcmp(path, "/gone")) { errno = ENOENT; return -1; }
    if (!strcmp(path, "/zero")) info->f_blocks = 0;
    if (!strcmp(path, "/overflow")) {
        info->f_blocks = ULLONG_MAX;
        info->f_frsize = 2;
    }
    return 0;
}

#define fopen fixture_fopen
#define statvfs(path, info) fixture_statvfs(path, info)
#include "../c/osif/linux/osinfo.c"
#undef statvfs
#undef fopen

typedef struct {
    osif_mount_t rows[8];
    size_t count;
    int stop;
} observed_t;

static int observe(const osif_mount_t* mount, void* user) {
    observed_t* observed = user;
    assert(observed->count < sizeof(observed->rows) / sizeof(observed->rows[0]));
    observed->rows[observed->count++] = *mount;
    return observed->stop;
}

static FILE* begin_fixture(void) {
    assert(!mount_fixture);
    mount_fixture = tmpfile();
    assert(mount_fixture);
    lookups = 0;
    last_lookup[0] = '\0';
    proc_missing = 0;
    return mount_fixture;
}

static int enumerate(observed_t* observed) {
    assert(!fflush(mount_fixture));
    rewind(mount_fixture);
    return bear_osinfo_mounts(observe, observed);
}

static void test_long_options_and_later_rows(void) {
    FILE* stream = begin_fixture();
    assert(fputs("proc /proc proc rw 0 0\noverlay / overlay rw,lowerdir=", stream) >= 0);
    for (int i = 0; i < 16384; i++) assert(fputc('x', stream) != EOF);
    assert(fputs(" 0 0\n/dev/sda /second ext4 rw 0 0\n", stream) >= 0);
    observed_t observed = {0};
    assert(!enumerate(&observed));
    assert(observed.count == 2 && lookups == 2);
    assert(!strcmp(observed.rows[0].mountpoint, "/"));
    assert(!strcmp(observed.rows[1].mountpoint, "/second"));
    assert(observed.rows[0].total_b == 409600);
    assert(observed.rows[0].free_b == 102400);
    assert(observed.rows[0].avail_b == 81920);
}

static void test_exact_escaped_fields(void) {
    FILE* stream = begin_fixture();
    assert(fputs("/dev/disk\\040name\\134tail "
                 "/mount\\040space\\011tab\\012line\\134slash ext4 rw 0 0\n",
                 stream) >= 0);
    observed_t observed = {0};
    assert(!enumerate(&observed));
    assert(observed.count == 1 && lookups == 1);
    assert(!strcmp(observed.rows[0].device, "/dev/disk name\\tail"));
    assert(!strcmp(observed.rows[0].mountpoint, "/mount space\ttab\nline\\slash"));
    assert(!strcmp(last_lookup, observed.rows[0].mountpoint));
}

static void test_invalid_row_is_not_success_or_end_of_table(void) {
    FILE* stream = begin_fixture();
    assert(fputs("malformed row\n/dev/a /valid ext4 rw 0 0\n", stream) >= 0);
    observed_t observed = {0};
    assert(enumerate(&observed) == -1 && errno == EINVAL);
    assert(observed.count == 1);
    assert(!strcmp(observed.rows[0].mountpoint, "/valid"));
}

static void test_capacity_is_checked_after_decoding(void) {
    FILE* stream = begin_fixture();
    assert(fputs("/dev/a /", stream) >= 0);
    for (int i = 0; i < 254; i++) assert(fputs("\\040", stream) >= 0);
    assert(fputs(" ext4 rw 0 0\n", stream) >= 0);
    observed_t observed = {0};
    assert(!enumerate(&observed));
    assert(observed.count == 1 && strlen(observed.rows[0].mountpoint) == 255);
    assert(observed.rows[0].mountpoint[254] == ' ');

    stream = begin_fixture();
    assert(fputs("/dev/a /", stream) >= 0);
    for (int i = 0; i < 255; i++) assert(fputc('p', stream) != EOF);
    assert(fputs(" ext4 rw 0 0\n/dev/b /valid ext4 rw 0 0\n", stream) >= 0);
    observed = (observed_t){0};
    assert(enumerate(&observed) == -1 && errno == ENAMETOOLONG);
    assert(observed.count == 1 && lookups == 1);
    assert(!strcmp(last_lookup, "/valid"));
}

static void test_missing_mount_empty_fs_and_fallback(void) {
    FILE* stream = begin_fixture();
    assert(fputs("# comment\n\n/dev/a /gone ext4 rw 0 0\n"
                 "/dev/b /zero ext4 rw 0 0\n/dev/c /valid ext4 rw 0 0",
                 stream) >= 0);
    proc_missing = 1;
    observed_t observed = {0};
    assert(!enumerate(&observed));
    assert(observed.count == 1 && lookups == 3);
    assert(!strcmp(observed.rows[0].mountpoint, "/valid"));
}

static void test_callback_stop_and_count_overflow(void) {
    FILE* stream = begin_fixture();
    assert(fputs("/dev/a /first ext4 rw 0 0\n/dev/b /second ext4 rw 0 0\n",
                 stream) >= 0);
    observed_t observed = {.stop = 1};
    assert(!enumerate(&observed));
    assert(observed.count == 1 && lookups == 1);

    stream = begin_fixture();
    assert(fputs("/dev/a /overflow ext4 rw 0 0\n/dev/b /valid ext4 rw 0 0\n",
                 stream) >= 0);
    observed = (observed_t){0};
    assert(enumerate(&observed) == -1 && errno == EOVERFLOW);
    assert(observed.count == 1);
}

static ssize_t read_failure(void* context, char* buffer, size_t capacity) {
    (void)context; (void)buffer; (void)capacity;
    errno = EIO;
    return -1;
}

static void test_read_error_and_nul_are_not_empty_success(void) {
    assert(!mount_fixture);
    cookie_io_functions_t functions = {.read = read_failure};
    mount_fixture = fopencookie(NULL, "r", functions);
    assert(mount_fixture);
    proc_missing = 0;
    observed_t observed = {0};
    assert(bear_osinfo_mounts(observe, &observed) == -1 && errno == EIO);
    assert(!observed.count);

    FILE* stream = begin_fixture();
    const char embedded[] = "/dev/a /truncated\0name ext4 rw 0 0\n";
    assert(fwrite(embedded, 1, sizeof(embedded) - 1, stream) == sizeof(embedded) - 1);
    assert(enumerate(&observed) == -1 && errno == EINVAL);
    assert(!observed.count && !lookups);
}

int main(void) {
    test_long_options_and_later_rows();
    test_exact_escaped_fields();
    test_invalid_row_is_not_success_or_end_of_table();
    test_capacity_is_checked_after_decoding();
    test_missing_mount_empty_fs_and_fallback();
    test_callback_stop_and_count_overflow();
    test_read_error_and_nul_are_not_empty_success();
    puts("linux mount provider: 7 regression groups passed");
    return 0;
}
