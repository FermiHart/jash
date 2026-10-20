// SPDX-License-Identifier: LicenseRef-SPL-1.0
#define _GNU_SOURCE
#include "buffer.h"
#include "ipc.h"
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

int jash_buffer_compiled(void) {
#if defined(__linux__) && !defined(JASH_BETHOS)
    return 1;
#else
    return 0;
#endif
}
int jash_buffer_validate(int fd, uint64_t size, uint64_t limit) {
#if defined(__linux__) && !defined(JASH_BETHOS)
    struct stat st;
    if (size > limit || size > SIZE_MAX || fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 0 || (uint64_t)st.st_size != size) { errno = EINVAL; return -1; }
    int seals = fcntl(fd, F_GET_SEALS), flags = fcntl(fd, F_GETFD);
    int required = F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL;
    if (seals < 0 || (seals & required) != required || flags < 0 || !(flags & FD_CLOEXEC)) { errno = EPERM; return -1; }
    return 0;
#else
    (void)fd; (void)size; (void)limit; errno = ENOTSUP; return -1;
#endif
}
int jash_buffer_create(const void* bytes, size_t size, uint64_t limit) {
    if ((size && !bytes) || size > limit) { errno = EINVAL; return -1; }
    return jash_ipc_snapshot_bytes(bytes, size);
}
int jash_buffer_probe(void) {
    int fd = jash_buffer_create("", 0, 0); if (fd < 0) return 0;
    int valid = !jash_buffer_validate(fd, 0, 0); close(fd); return valid;
}
int jash_buffer_map(int fd, uint64_t size, uint64_t limit, const void** address) {
    if (!address) { errno = EINVAL; return -1; } *address = NULL;
    if (jash_buffer_validate(fd, size, limit)) return -1;
    if (!size) return 0;
    /* The sealed file was validated above. A read-only private mapping sees
     * the same immutable bytes without requiring a new shared mapping after
     * F_SEAL_WRITE (refused by the Tina guest). Never expose a writable map. */
    void* map = mmap(NULL, (size_t)size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) return -1;
    *address = map; return 0;
}
void jash_buffer_unmap(const void* address, uint64_t size) { if (address && size) (void)munmap((void*)address, (size_t)size); }
int jash_buffer_reader_fd(int source, uint64_t size, uint64_t limit) {
#if defined(__linux__) && !defined(JASH_BETHOS)
    struct stat before, opened, after;
    if (jash_buffer_validate(source, size, limit) || fstat(source, &before)) return -1;
    char path[64];
    int n = snprintf(path, sizeof(path), "/proc/self/fd/%d", source);
    if (n <= 0 || (size_t)n >= sizeof(path)) { errno = ENAMETOOLONG; return -1; }
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0 || (flags & O_ACCMODE) != O_RDONLY ||
        fstat(fd, &opened) || opened.st_dev != before.st_dev ||
        opened.st_ino != before.st_ino || opened.st_size != before.st_size ||
        jash_buffer_validate(fd, size, limit) || fstat(source, &after) ||
        after.st_dev != before.st_dev || after.st_ino != before.st_ino ||
        after.st_size != before.st_size || lseek(fd, 0, SEEK_SET) != 0) {
        int saved = errno ? errno : ESTALE;
        close(fd); errno = saved; return -1;
    }
    return fd;
#else
    (void)source; (void)size; (void)limit; errno = ENOTSUP; return -1;
#endif
}
int jash_buffer_copy_fd(int source, uint64_t limit) {
#if defined(__linux__) && !defined(JASH_BETHOS)
    struct stat before, after;
    if (fstat(source, &before) || !S_ISREG(before.st_mode) || before.st_size < 0 || (uint64_t)before.st_size > limit) { errno = EINVAL; return -1; }
    int fd = memfd_create("jash-data-copy", MFD_CLOEXEC | MFD_ALLOW_SEALING); if (fd < 0) return -1;
    char bytes[65536]; off_t offset = 0;
    while (offset < before.st_size) {
        size_t take = before.st_size - offset > (off_t)sizeof(bytes) ? sizeof(bytes) : (size_t)(before.st_size - offset);
        ssize_t n = pread(source, bytes, take, offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) goto failed;
        size_t used = 0;
        while (used < (size_t)n) {
            ssize_t written = write(fd, bytes + used, (size_t)n - used);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) goto failed;
            used += (size_t)written;
        }
        offset += n;
    }
    if (fstat(source, &after) || before.st_dev != after.st_dev || before.st_ino != after.st_ino || before.st_size != after.st_size ||
        before.st_mtim.tv_sec != after.st_mtim.tv_sec || before.st_mtim.tv_nsec != after.st_mtim.tv_nsec ||
        before.st_ctim.tv_sec != after.st_ctim.tv_sec || before.st_ctim.tv_nsec != after.st_ctim.tv_nsec) { errno = ESTALE; goto failed; }
    if (fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL)) goto failed;
    return fd;
failed: { int saved = errno ? errno : EIO; close(fd); errno = saved; return -1; }
#else
    (void)source; (void)limit; errno = ENOTSUP; return -1;
#endif
}

int jash_buffer_copy_range_fd(int source, uint64_t offset, uint64_t length,
                              uint64_t limit) {
#if defined(__linux__) && !defined(JASH_BETHOS)
    struct stat before, after;
    if (fstat(source, &before) || !S_ISREG(before.st_mode) ||
        before.st_size < 0 || length > limit ||
        offset > (uint64_t)before.st_size ||
        length > (uint64_t)before.st_size - offset) {
        errno = EINVAL;
        return -1;
    }
    int fd = memfd_create("jash-data-range", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0) return -1;
    char bytes[65536];
    uint64_t copied = 0;
    while (copied < length) {
        size_t take = length - copied > sizeof bytes
            ? sizeof bytes : (size_t)(length - copied);
        ssize_t n = pread(source, bytes, take, (off_t)(offset + copied));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) goto failed;
        size_t used = 0;
        while (used < (size_t)n) {
            ssize_t written = write(fd, bytes + used, (size_t)n - used);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) goto failed;
            used += (size_t)written;
        }
        copied += (uint64_t)n;
    }
    if (fstat(source, &after) || before.st_dev != after.st_dev ||
        before.st_ino != after.st_ino || before.st_size != after.st_size ||
        before.st_mtim.tv_sec != after.st_mtim.tv_sec ||
        before.st_mtim.tv_nsec != after.st_mtim.tv_nsec ||
        before.st_ctim.tv_sec != after.st_ctim.tv_sec ||
        before.st_ctim.tv_nsec != after.st_ctim.tv_nsec) {
        errno = ESTALE;
        goto failed;
    }
    if (fcntl(fd, F_ADD_SEALS,
              F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL))
        goto failed;
    return fd;
failed: {
        int saved = errno ? errno : EIO;
        close(fd);
        errno = saved;
        return -1;
    }
#else
    (void)source;
    (void)offset;
    (void)length;
    (void)limit;
    errno = ENOTSUP;
    return -1;
#endif
}

int jash_buffer_copy_observed_range_fd(int source,uint64_t source_size,
    uint64_t offset,uint64_t length,uint64_t limit,jash_buffer_observer observer,
    void *user,jash_buffer_copy_stats *stats) {
    if(stats)memset(stats,0,sizeof(*stats));
#if defined(__linux__) && !defined(JASH_BETHOS)
    struct stat before,after;
    if(!observer||source_size>limit||length>limit||offset>source_size||
       length>source_size-offset||fstat(source,&before)||!S_ISREG(before.st_mode)||
       before.st_size<0||(uint64_t)before.st_size!=source_size) {
        errno=EINVAL;return -1;
    }
    int fd=memfd_create("jash-verified-range",MFD_CLOEXEC|MFD_ALLOW_SEALING);
    if(fd<0)return -1;
    unsigned char bytes[65536];
    uint64_t at=0;
    while(at<source_size) {
        size_t take=source_size-at>sizeof(bytes)?sizeof(bytes):(size_t)(source_size-at);
        ssize_t n=pread(source,bytes,take,(off_t)at);
        if(n<0&&errno==EINTR)continue;
        if(n<=0) { errno=EIO;goto failed; }
        if(stats)stats->read_bytes+=(uint64_t)n;
        if(observer(user,bytes,(size_t)n,at)) { if(!errno)errno=EBADMSG;goto failed; }
        uint64_t first=at>offset?at:offset;
        uint64_t last=at+(uint64_t)n<offset+length?at+(uint64_t)n:offset+length;
        while(first<last) {
            ssize_t written=write(fd,bytes+(size_t)(first-at),(size_t)(last-first));
            if(written<0&&errno==EINTR)continue;
            if(written<=0) { errno=EIO;goto failed; }
            first+=(uint64_t)written;
            if(stats)stats->copied_bytes+=(uint64_t)written;
        }
        at+=(uint64_t)n;
    }
    if(fstat(source,&after)||before.st_dev!=after.st_dev||before.st_ino!=after.st_ino||
       before.st_size!=after.st_size||before.st_mtim.tv_sec!=after.st_mtim.tv_sec||
       before.st_mtim.tv_nsec!=after.st_mtim.tv_nsec||before.st_ctim.tv_sec!=after.st_ctim.tv_sec||
       before.st_ctim.tv_nsec!=after.st_ctim.tv_nsec) { errno=ESTALE;goto failed; }
    if(fcntl(fd,F_ADD_SEALS,F_SEAL_WRITE|F_SEAL_GROW|F_SEAL_SHRINK|F_SEAL_SEAL))goto failed;
    return fd;
failed: {
        int saved=errno?errno:EIO;close(fd);errno=saved;return -1;
    }
#else
    (void)source;(void)source_size;(void)offset;(void)length;(void)limit;
    (void)observer;(void)user;errno=ENOTSUP;return -1;
#endif
}
