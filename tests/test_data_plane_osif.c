// SPDX-License-Identifier: LicenseRef-SPL-1.0
#define _GNU_SOURCE
#include <osif/ipc.h>
#include <osif/buffer.h>
#include <module/blake3.h>
#include <assert.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <time.h>

static int descriptors(void) {
    DIR* d = opendir("/proc/self/fd"); assert(d); int n = 0; while (readdir(d)) n++; closedir(d); return n;
}
static uint64_t ns(void) { struct timespec t; assert(!clock_gettime(CLOCK_MONOTONIC, &t)); return (uint64_t)t.tv_sec * 1000000000u + t.tv_nsec; }
typedef struct { struct blake3 hash; uint64_t read; int mutate_fd,refuse; } observed;
static int observe(void *user,const void *bytes,size_t length,uint64_t offset) {
    observed *o=user;assert(offset==o->read);o->read+=length;
    blake3_update(&o->hash,bytes,length);
    if(o->mutate_fd>=0) { assert(!ftruncate(o->mutate_fd,1));o->mutate_fd=-1; }
    if(o->refuse) { errno=EBADMSG;return -1; }
    return 0;
}
static void test_observed_range(void) {
    unsigned char bytes[140003];
    for(size_t i=0;i<sizeof bytes;i++)bytes[i]=(unsigned char)(i*37u);
    int source=memfd_create("observed",MFD_CLOEXEC|MFD_ALLOW_SEALING);assert(source>=0);
    assert(write(source,bytes,sizeof bytes)==(ssize_t)sizeof bytes);
    unsigned char expected[32],digest[32];struct blake3 full;
    blake3_init(&full);blake3_update(&full,bytes,sizeof bytes);blake3_final(&full,expected,32);
    const uint64_t offsets[]={0,65530,sizeof bytes};
    for(size_t i=0;i<sizeof offsets/sizeof offsets[0];i++) {
        uint64_t offset=offsets[i],length=sizeof bytes-offset;
        observed o={0};o.mutate_fd=-1;blake3_init(&o.hash);jash_buffer_copy_stats stats;
        int out=jash_buffer_copy_observed_range_fd(source,sizeof bytes,offset,length,sizeof bytes,observe,&o,&stats);
        assert(out>=0&&!jash_buffer_validate(out,length,sizeof bytes));
        assert(stats.read_bytes==sizeof bytes&&stats.copied_bytes==length&&o.read==sizeof bytes);
        blake3_final(&o.hash,digest,32);assert(!memcmp(digest,expected,32));
        unsigned char check[sizeof bytes];assert(pread(out,check,(size_t)length,0)==(ssize_t)length);
        assert(!memcmp(check,bytes+offset,(size_t)length));close(out);
    }
    observed o={0};o.mutate_fd=-1;o.refuse=1;blake3_init(&o.hash);jash_buffer_copy_stats stats;
    assert(jash_buffer_copy_observed_range_fd(source,sizeof bytes,0,sizeof bytes,sizeof bytes,observe,&o,&stats)<0&&errno==EBADMSG);
    assert(stats.read_bytes==65536&&stats.copied_bytes==0);
    o.refuse=0;o.read=0;o.mutate_fd=source;blake3_init(&o.hash);
    assert(jash_buffer_copy_observed_range_fd(source,sizeof bytes,0,sizeof bytes,sizeof bytes,observe,&o,&stats)<0);
    assert(stats.copied_bytes<sizeof bytes);close(source);
}
int main(int argc, char** argv) {
    (void)argv;
    assert(jash_buffer_compiled() && jash_buffer_probe());
    int baseline = descriptors(), pair[2]; assert(!jash_ipc_data_pair(pair));
    test_observed_range();
    int fd = jash_buffer_create("hello", 5, 5); assert(fd >= 0 && !jash_buffer_validate(fd, 5, 5));
    assert(jash_buffer_validate(fd, 4, 5) && jash_buffer_validate(fd, 5, 4));
    assert(write(fd, "x", 1) < 0 && ftruncate(fd, 0) < 0);
    assert(mmap(NULL, 5, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) == MAP_FAILED);
    const void* map; assert(!jash_buffer_map(fd, 5, 5, &map) && !memcmp(map, "hello", 5)); jash_buffer_unmap(map, 5);
    assert(lseek(fd, 5, SEEK_SET) == 5);
    int reader = jash_buffer_reader_fd(fd, 5, 5); assert(reader >= 0);
    char reader_bytes[5]; assert(read(reader, reader_bytes, sizeof(reader_bytes)) == 5);
    assert(!memcmp(reader_bytes, "hello", 5) && lseek(fd, 0, SEEK_CUR) == 5);
    close(reader);
    int mutable = memfd_create("mutable", MFD_CLOEXEC | MFD_ALLOW_SEALING); assert(mutable >= 0 && write(mutable, "hello", 5) == 5);
    void* writable = mmap(NULL, 5, PROT_READ | PROT_WRITE, MAP_SHARED, mutable, 0); assert(writable != MAP_FAILED);
    assert(fcntl(mutable, F_ADD_SEALS, F_SEAL_WRITE) < 0); munmap(writable, 5);
    assert(jash_buffer_validate(mutable, 5, 5));
    int copy = jash_buffer_copy_fd(mutable, 5); assert(copy >= 0 && !jash_buffer_validate(copy, 5, 5));
    int range = jash_buffer_copy_range_fd(copy, 1, 3, 3);
    assert(range >= 0 && !jash_buffer_validate(range, 3, 3));
    char range_bytes[3]; assert(pread(range, range_bytes, 3, 0) == 3);
    assert(!memcmp(range_bytes, "ell", 3)); close(range);
    assert(jash_buffer_copy_range_fd(copy, 4, 2, 3) < 0 && errno == EINVAL);
    assert(lseek(mutable, 0, SEEK_CUR) == 5); close(copy); close(mutable);
    assert(!jash_ipc_data_send(pair[0], "transfer", 5, fd));
    uint64_t size; int received; assert(!jash_ipc_data_receive(pair[1], "transfer", &size, &received) && size == 5);
    assert(fcntl(received, F_GETFD) & FD_CLOEXEC); assert(!jash_buffer_validate(received, 5, 5)); close(received);
    assert(jash_ipc_data_receive(pair[1], "transfer", &size, &received) && received == -1 && errno == EAGAIN);
    const unsigned char binary[] = {'a', 0, 0xff, 'z'};
    assert(!jash_ipc_stream_send(pair[0], "s-test", 7, 11, JASH_IPC_STREAM_DATA, binary, sizeof(binary)));
    unsigned char output[JASH_IPC_STREAM_CHUNK_MAX]; jash_ipc_stream_frame_t stream;
    assert(!jash_ipc_stream_receive(pair[1], &stream, output, sizeof(output)));
    assert(!strcmp(stream.ref, "s-test") && stream.sequence == 7 && stream.offset == 11);
    assert(stream.flags == JASH_IPC_STREAM_DATA && stream.length == sizeof(binary));
    assert(!memcmp(output, binary, sizeof(binary)));
    assert(!jash_ipc_stream_send(pair[0], "s-test", 8, 15, JASH_IPC_STREAM_EOF, NULL, 0));
    assert(!jash_ipc_stream_receive(pair[1], &stream, output, sizeof(output)));
    assert(stream.flags == JASH_IPC_STREAM_EOF && stream.length == 0);
    unsigned char bad_stream[JASH_IPC_STREAM_HEADER_SIZE] = {0};
    memcpy(bad_stream, "JDS1", 4); bad_stream[4] = JASH_IPC_STREAM_EOF;
    bad_stream[5] = 6; memcpy(bad_stream + 32, "s-test", 6);
    struct iovec bad_payload = {bad_stream, sizeof bad_stream};
    union { struct cmsghdr align; char data[CMSG_SPACE(sizeof(int))]; } bad_control = {0};
    struct msghdr bad_message = {0}; bad_message.msg_iov = &bad_payload; bad_message.msg_iovlen = 1;
    bad_message.msg_control = bad_control.data; bad_message.msg_controllen = sizeof bad_control.data;
    struct cmsghdr* bad_cmsg = CMSG_FIRSTHDR(&bad_message); bad_cmsg->cmsg_level = SOL_SOCKET;
    bad_cmsg->cmsg_type = SCM_RIGHTS; bad_cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(bad_cmsg), &fd, sizeof(fd));
    assert(sendmsg(pair[0], &bad_message, 0) == (ssize_t)sizeof bad_stream);
    assert(jash_ipc_stream_receive(pair[1], &stream, output, sizeof(output)) && errno == EPROTO);
    int count = descriptors();
    for (int iteration = 0; iteration < 100; iteration++) {
        assert(!jash_ipc_data_send(pair[0], "wrong", 5, fd));
        assert(jash_ipc_data_receive(pair[1], "expected", &size, &received) && received == -1);
        unsigned char frame[100] = {0}; memcpy(frame, "JDF1", 4); frame[4] = 1; frame[8] = 'x';
        struct iovec payload = {frame, sizeof(frame)};
        union { struct cmsghdr align; char data[CMSG_SPACE(20 * sizeof(int))]; } control = {0};
        struct msghdr message = {0}; message.msg_iov = &payload; message.msg_iovlen = 1; message.msg_control = control.data; message.msg_controllen = sizeof(control.data);
        struct cmsghdr* cmsg = CMSG_FIRSTHDR(&message); cmsg->cmsg_level = SOL_SOCKET; cmsg->cmsg_type = SCM_RIGHTS; cmsg->cmsg_len = CMSG_LEN(20 * sizeof(int));
        for (int i = 0; i < 20; i++) memcpy((char*)CMSG_DATA(cmsg) + i * sizeof(int), &fd, sizeof(fd));
        assert(sendmsg(pair[0], &message, 0) == 100);
        assert(jash_ipc_data_receive(pair[1], "x", &size, &received) && received == -1);
    }
    assert(descriptors() == count);
    if (argc > 1) {
        size_t sizes[] = {256, 65536, 1048576}; printf("{\"schema\":\"jash.transport-baseline.v1\",\"samples\":[");
        for (int s = 0; s < 3; s++) {
            size_t n = sizes[s]; char* bytes = malloc(n); assert(bytes); memset(bytes, 'x', n);
            uint64_t create = 0, transfer = 0; unsigned char hash[32];
            for (int i = 0; i < 20; i++) {
                uint64_t begin = ns(); int object = jash_buffer_create(bytes, n, n); assert(object >= 0); create += ns() - begin;
                begin = ns(); assert(!jash_ipc_data_send(pair[0], "benchmark", n, object));
                assert(!jash_ipc_data_receive(pair[1], "benchmark", &size, &received));
                assert(!jash_buffer_map(received, n, n, &map));
                struct blake3 state; blake3_init(&state); blake3_update(&state, map, n); blake3_final(&state, hash, sizeof(hash)); jash_buffer_unmap(map, n);
                transfer += ns() - begin; close(received); close(object);
            }
            printf("%s{\"bytes\":%zu,\"iterations\":20,\"create_seal_ns\":%llu,\"transfer_map_hash_ns\":%llu,\"creation_payload_copied_bytes\":%zu,\"transfer_adapter_payload_copied_bytes\":0}", s ? "," : "", n,
                   (unsigned long long)create, (unsigned long long)transfer, n * 20); free(bytes);
        }
        puts("]}");
    }
    close(fd); close(pair[0]); close(pair[1]); assert(descriptors() == baseline); return 0;
}
