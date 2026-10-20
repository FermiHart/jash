// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash ring-3 boundary regression probe for SinguOS DEV-LAB.

#include "builtin.h"

#ifndef JASH_TARGET_BEYOND

int jash_b_ring3_selftest(jash_state_t* s, jash_writer_t w, void* ctx,
                          const char* args) {
    (void)s;
    (void)args;
    jash_writes(w, ctx, "ring3-selftest: unavailable outside Beyond\n");
    return 1;
}

#else

#include <singuos/genesis_v1.h>
#include <singuos/execution_object_v1.h>
#include <singuos/nerva_stream_v1.h>
#include "blake3.h"

extern long bear_raw_syscall6(long nr, long a1, long a2, long a3,
                              long a4, long a5, long a6);

#define BEY_READ          0x60
#define BEY_PLEDGE        0x00
#define BEY_UNVEIL        0x01
#define BEY_IDENTITY_GET  0x10
#define BEY_IDENTITY_SET  0x11
#define BEY_NERVA_SUBMIT  0x12
#define BEY_NERVA_POLL    0x13
#define BEY_NERVA_SQ_BIND 0x14
#define BEY_NERVA_SQ_KICK 0x15
#define BEY_NERVA_SQ_UNBIND 0x16
#define BEY_WRITE         0x61
#define BEY_IOCTL         0x67
#define BEY_FSTAT         0x84
#define BEY_CHDIR         0x86
#define BEY_ARCH_PRCTL    0xE0
#define BEY_CELL_EMIT     0x140
#define BEY_MMAP          0x40
#define BEY_MUNMAP        0x41
#define BEY_MPROTECT      0x42
#define BEY_SURFACE_CREATE 0x153
#define ARCH_GET_FS       0x1003
#define EXPECT_EFAULT     (-14L)
#define EXPECT_ENOSYS     (-38L)
#define EXPECT_ESTALE     (-116L)

static int expect_result(jash_writer_t w, void* ctx, const char* name,
                         long result, long expected) {
    if (result == expected) return 0;
    jash_writef(w, ctx, "RING3_SELFTEST FAIL %s got=%ld expected=%ld\n",
                name, result, expected);
    return 1;
}

static int expect_refused(jash_writer_t w, void* ctx, const char* name,
                          long result) {
    if (result < 0) return 0;
    jash_writef(w, ctx, "RING3_SELFTEST FAIL %s accepted=%ld\n", name, result);
    return 1;
}

int jash_b_ring3_selftest(jash_state_t* s, jash_writer_t w, void* ctx,
                          const char* args) {
    int failed = 0;
    const long bad = 1;
    const long kernel = (long)0xffffffff80100000ULL;
    static const char probe[] = "x";
    struct singuos_exec_identity_v1 identity;
    struct nerva_v1_record nerva_input;
    struct nerva_v1_record nerva_output;
    struct nerva_v1_submission_page* nerva_sq_page;
    long surface_handle;
    long sq_address;
    long sq_generation;
    long replacement_generation;
    unsigned int i;
    int principal_nonzero = 0;
    int nerva_found = 0;
    int nerva_syscall_seen = 0;
    int nerva_memory_seen = 0;
    int nerva_device_seen = 0;

    (void)s;
    (void)args;
    failed += expect_result(w, ctx, "read-bad", bear_raw_syscall6(
        BEY_READ, 0, bad, 1, 0, 0, 0), EXPECT_EFAULT);
    failed += expect_result(w, ctx, "pledge-unsupported", bear_raw_syscall6(
        BEY_PLEDGE, 0, 0, 0, 0, 0, 0), EXPECT_ENOSYS);
    failed += expect_result(w, ctx, "unveil-unsupported", bear_raw_syscall6(
        BEY_UNVEIL, 0, 0, 0, 0, 0, 0), EXPECT_ENOSYS);
    failed += expect_result(w, ctx, "write-bad", bear_raw_syscall6(
        BEY_WRITE, 1, bad, 1, 0, 0, 0), EXPECT_EFAULT);
    failed += expect_result(w, ctx, "ioctl-kernel", bear_raw_syscall6(
        BEY_IOCTL, 1, SINGUOS_GENESIS_GET_V1, kernel, 0, 0, 0), EXPECT_EFAULT);
    failed += expect_result(w, ctx, "fstat-bad", bear_raw_syscall6(
        BEY_FSTAT, 1, bad, 0, 0, 0, 0), EXPECT_EFAULT);
    failed += expect_result(w, ctx, "chdir-bad", bear_raw_syscall6(
        BEY_CHDIR, bad, 0, 0, 0, 0, 0), EXPECT_EFAULT);
    failed += expect_result(w, ctx, "arch-get-bad", bear_raw_syscall6(
        BEY_ARCH_PRCTL, ARCH_GET_FS, bad, 0, 0, 0, 0), EXPECT_EFAULT);
    failed += expect_result(w, ctx, "emit-bad", bear_raw_syscall6(
        BEY_CELL_EMIT, 1, 1, bad, 1, 0, 0), EXPECT_EFAULT);
    failed += expect_refused(w, ctx, "munmap-code", bear_raw_syscall6(
        BEY_MUNMAP, 0x600000, 4096, 0, 0, 0, 0));
    failed += expect_refused(w, ctx, "mprotect-code", bear_raw_syscall6(
        BEY_MPROTECT, 0x600000, 4096, 1, 0, 0, 0));
    failed += expect_refused(w, ctx, "mmap-wx", bear_raw_syscall6(
        BEY_MMAP, 0, 4096, 6, 0x22, -1, 0));
    failed += expect_result(w, ctx, "identity-get", bear_raw_syscall6(
        BEY_IDENTITY_GET, (long)&identity, 0, 0, 0, 0, 0), 0);
    if (identity.version != SINGUOS_EXEC_ABI_VERSION ||
        identity.size != sizeof(identity) ||
        identity.kind != SINGUOS_EXEC_KIND_COMPAT_SURFACE ||
        identity.handle == 0) {
        jash_writes(w, ctx, "RING3_SELFTEST FAIL identity-shape\n");
        failed++;
    }
    for (i = 0; i < SINGUOS_EXEC_ID_SIZE; ++i)
        principal_nonzero |= identity.principal_id[i] != 0;
    if (!principal_nonzero) {
        jash_writes(w, ctx, "RING3_SELFTEST FAIL principal-zero\n");
        failed++;
    }
    failed += expect_refused(w, ctx, "identity-set", bear_raw_syscall6(
        BEY_IDENTITY_SET, (long)&identity, 0, 0, 0, 0, 0));
    failed += expect_result(w, ctx, "nerva-submit-bad", bear_raw_syscall6(
        BEY_NERVA_SUBMIT, bad, 0, 0, 0, 0, 0), EXPECT_EFAULT);
    failed += expect_result(w, ctx, "nerva-poll-bad", bear_raw_syscall6(
        BEY_NERVA_POLL, bad, 0, 0, 0, 0, 0), EXPECT_EFAULT);
    for (i = 0; i < sizeof(nerva_input); ++i)
        ((unsigned char*)&nerva_input)[i] = 0;
    nerva_input.magic[0] = 'S'; nerva_input.magic[1] = 'G';
    nerva_input.magic[2] = 'N'; nerva_input.magic[3] = 'E';
    nerva_input.magic[4] = 'R'; nerva_input.magic[5] = 'V';
    nerva_input.magic[6] = 'A'; nerva_input.magic[7] = '1';
    nerva_input.version_major = NERVA_V1_VERSION_MAJOR;
    nerva_input.version_minor = NERVA_V1_VERSION_MINOR;
    nerva_input.record_size = NERVA_V1_RECORD_SIZE;
    nerva_input.record_type = NERVA_V1_RECORD_INTENTION;
    nerva_input.stream_id = NERVA_V1_STREAM_CONTROL;
    nerva_input.request_id[0] = 1;
    nerva_input.intent_id[0] = 2;
    nerva_input.payload_schema_hash[0] = 1;
    nerva_input.payload_length = 1;
    nerva_input.payload[0] = 0x5a;
    blake3_hash(nerva_input.payload, nerva_input.payload_length,
                nerva_input.payload_hash);
    failed += expect_result(w, ctx, "nerva-submit", bear_raw_syscall6(
        BEY_NERVA_SUBMIT, (long)&nerva_input, 0, 0, 0, 0, 0), 0);
    for (i = 0; i < NERVA_V1_QUEUE_CAPACITY + 2; ++i) {
        long poll_result = bear_raw_syscall6(
            BEY_NERVA_POLL, (long)&nerva_output, 0, 0, 0, 0, 0);
        if (poll_result != 0) {
            failed += expect_result(w, ctx, "nerva-poll", poll_result, 0);
            break;
        }
        if (nerva_output.record_type == NERVA_V1_RECORD_COMPLETION &&
            nerva_output.request_id[0] == nerva_input.request_id[0] &&
            nerva_output.intent_id[0] == nerva_input.intent_id[0]) {
            nerva_found = 1;
            break;
        }
        if (nerva_output.stream_id == NERVA_V1_STREAM_SYSCALL)
            nerva_syscall_seen = 1;
        if (nerva_output.stream_id == NERVA_V1_STREAM_MEMORY)
            nerva_memory_seen = 1;
        if (nerva_output.stream_id == NERVA_V1_STREAM_DEVICE)
            nerva_device_seen = 1;
        if (nerva_output.record_type == NERVA_V1_RECORD_INTENTION ||
            nerva_output.trust_class == NERVA_V1_TRUST_UNTRUSTED) {
            jash_writes(w, ctx, "RING3_SELFTEST FAIL nerva-untrusted-output\n");
            failed++;
            break;
        }
    }
    if (!nerva_found ||
        nerva_output.record_type != NERVA_V1_RECORD_COMPLETION ||
        nerva_output.status != NERVA_V1_STATUS_ACCEPTED ||
        nerva_output.trust_class != NERVA_V1_TRUST_KERNEL_OBSERVED ||
        nerva_output.sequence == 0 ||
        nerva_output.principal_id[0] != identity.principal_id[0] ||
        nerva_output.object_id[0] != identity.object_id[0]) {
        jash_writes(w, ctx, "RING3_SELFTEST FAIL nerva-completion-shape\n");
        failed++;
    }
    if (!nerva_syscall_seen || !nerva_memory_seen || !nerva_device_seen) {
        jash_writes(w, ctx, "RING3_SELFTEST FAIL nerva-live-streams\n");
        failed++;
    }
    failed += expect_result(w, ctx, "nerva-sq-bind-bad", bear_raw_syscall6(
        BEY_NERVA_SQ_BIND, bad, 0, 0, 0, 0, 0), EXPECT_EFAULT);
    sq_address = bear_raw_syscall6(BEY_MMAP, 0,
        NERVA_V1_SUBMISSION_PAGE_SIZE, 3, 0x22, -1, 0);
    if (sq_address <= 0) {
        jash_writef(w, ctx, "RING3_SELFTEST FAIL nerva-sq-mmap got=%ld\n",
                    sq_address);
        return 1;
    }
    nerva_sq_page = (struct nerva_v1_submission_page*)sq_address;
    sq_generation = bear_raw_syscall6(
        BEY_NERVA_SQ_BIND, (long)nerva_sq_page, 0, 0, 0, 0, 0);
    failed += expect_result(w, ctx, "nerva-sq-bind", sq_generation, 1);
    nerva_input.request_id[0] = 3;
    nerva_input.intent_id[0] = 4;
    nerva_input.payload[0] = 0x6b;
    blake3_hash(nerva_input.payload, nerva_input.payload_length,
                nerva_input.payload_hash);
    nerva_sq_page->records[0] = nerva_input;
    nerva_sq_page->header.tail = 1;
    failed += expect_result(w, ctx, "nerva-sq-kick", bear_raw_syscall6(
        BEY_NERVA_SQ_KICK, (long)nerva_sq_page, sq_generation, 1, 0, 0, 0),
        1);
    nerva_found = 0;
    for (i = 0; i < NERVA_V1_QUEUE_CAPACITY + 2; ++i) {
        long poll_result = bear_raw_syscall6(
            BEY_NERVA_POLL, (long)&nerva_output, 0, 0, 0, 0, 0);
        if (poll_result != 0) {
            failed += expect_result(w, ctx, "nerva-sq-poll", poll_result, 0);
            break;
        }
        if (nerva_output.record_type == NERVA_V1_RECORD_COMPLETION &&
            nerva_output.request_id[0] == 3 &&
            nerva_output.intent_id[0] == 4) {
            nerva_found = 1;
            break;
        }
    }
    if (!nerva_found) {
        jash_writes(w, ctx, "RING3_SELFTEST FAIL nerva-sq-completion\n");
        failed++;
    }
    failed += expect_result(w, ctx, "nerva-sq-unbind-stale",
        bear_raw_syscall6(BEY_NERVA_SQ_UNBIND, (long)nerva_sq_page,
                          sq_generation + 1, 0, 0, 0, 0), EXPECT_ESTALE);
    failed += expect_result(w, ctx, "nerva-sq-unbind", bear_raw_syscall6(
        BEY_NERVA_SQ_UNBIND, (long)nerva_sq_page, sq_generation,
        0, 0, 0, 0), 0);
    failed += expect_result(w, ctx, "nerva-sq-kick-stale", bear_raw_syscall6(
        BEY_NERVA_SQ_KICK, (long)nerva_sq_page, sq_generation, 1,
        0, 0, 0), EXPECT_ESTALE);
    replacement_generation = bear_raw_syscall6(
        BEY_NERVA_SQ_BIND, (long)nerva_sq_page, 0, 0, 0, 0, 0);
    failed += expect_result(w, ctx, "nerva-sq-rebind", replacement_generation,
                            sq_generation + 1);
    failed += expect_result(w, ctx, "nerva-sq-final-unbind", bear_raw_syscall6(
        BEY_NERVA_SQ_UNBIND, (long)nerva_sq_page, replacement_generation,
        0, 0, 0, 0), 0);
    failed += expect_result(w, ctx, "nerva-sq-munmap", bear_raw_syscall6(
        BEY_MUNMAP, sq_address, NERVA_V1_SUBMISSION_PAGE_SIZE,
        0, 0, 0, 0), 0);
    surface_handle = bear_raw_syscall6(BEY_SURFACE_CREATE, 0, 0, 0, 0, 0, 0);
    failed += expect_result(w, ctx, "surface-handle", surface_handle,
                            (long)identity.handle);
    failed += expect_refused(w, ctx, "forged-surface", bear_raw_syscall6(
        BEY_CELL_EMIT, surface_handle ^ (1L << 16), 1, (long)probe, 1,
        0, 0));

    if (failed) return 1;
    for (unsigned int cycle = 0; cycle < 8; ++cycle) {
        long address = bear_raw_syscall6(BEY_MMAP, 0, 8192, 3, 0x22, -1, 0);
        volatile unsigned char *bytes;
        if (address <= 0) {
            jash_writes(w, ctx, "RING3_SELFTEST FAIL memory-mmap\n");
            return 1;
        }
        bytes = (volatile unsigned char *)address;
        for (i = 0; i < 8192; ++i) {
            if (bytes[i] != 0) {
                jash_writes(w, ctx, "RING3_SELFTEST FAIL memory-zero-fill\n");
                failed++;
                break;
            }
        }
        for (i = 0; i < 8192; ++i) bytes[i] = 0xa5;
        failed += expect_result(w, ctx, "memory-readonly", bear_raw_syscall6(
            BEY_MPROTECT, address, 8192, 1, 0, 0, 0), 0);
        failed += expect_result(w, ctx, "memory-deny-copyout", bear_raw_syscall6(
            BEY_IDENTITY_GET, address, 0, 0, 0, 0, 0), EXPECT_EFAULT);
        failed += expect_result(w, ctx, "memory-none", bear_raw_syscall6(
            BEY_MPROTECT, address, 8192, 0, 0, 0, 0), 0);
        failed += expect_result(w, ctx, "memory-deny-copyin", bear_raw_syscall6(
            BEY_WRITE, 1, address, 1, 0, 0, 0), EXPECT_EFAULT);
        failed += expect_result(w, ctx, "memory-rw", bear_raw_syscall6(
            BEY_MPROTECT, address, 8192, 3, 0, 0, 0), 0);
        failed += expect_result(w, ctx, "memory-protect-overflow", bear_raw_syscall6(
            BEY_MPROTECT, address, -1, 1, 0, 0, 0), -22);
        failed += expect_result(w, ctx, "memory-unmap-overflow", bear_raw_syscall6(
            BEY_MUNMAP, address, -1, 0, 0, 0, 0), -22);
        failed += expect_result(w, ctx, "memory-unmap", bear_raw_syscall6(
            BEY_MUNMAP, address, 8192, 0, 0, 0, 0), 0);
        failed += expect_result(w, ctx, "memory-released-copyin", bear_raw_syscall6(
            BEY_WRITE, 1, address, 1, 0, 0, 0), EXPECT_EFAULT);
        failed += expect_refused(w, ctx, "memory-double-unmap", bear_raw_syscall6(
            BEY_MUNMAP, address, 8192, 0, 0, 0, 0));
        if (failed) return 1;
    }
    jash_writes(w, ctx, "RING3_MEMORY PASS cycles=8\n");
    jash_writes(w, ctx, "RING3_SELFTEST PASS cases=30\n");
    return 0;
}

#endif
