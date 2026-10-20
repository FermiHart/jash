// SPDX-License-Identifier: LicenseRef-SPL-1.0
/* Includes the implementation only to exercise backend selection and the
 * compression boundary. Public streaming/HMAC output uses Python oracles. */
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__)) && \
    !defined(JASH_TARGET_BEYOND) && !defined(JASH_SHA256_PORTABLE_ONLY)
#include <cpuid.h>
static int host_has_sha(void) {
    unsigned a, b, c, d;
    if (!__get_cpuid(1, &a, &b, &c, &d)) return 0;
    if ((c & (bit_SSSE3 | bit_SSE4_1)) != (bit_SSSE3 | bit_SSE4_1)) return 0;
    return __get_cpuid_count(7, 0, &a, &b, &c, &d) && (b & bit_SHA);
}
static unsigned feature_mask, missing_leaf, queries;
static int test_cpuid(unsigned leaf, unsigned* a, unsigned* b, unsigned* c, unsigned* d) {
    __atomic_fetch_add(&queries, 1, __ATOMIC_RELAXED);
    *a = *b = *c = *d = 0;
    if (missing_leaf == leaf) return 0;
    if (leaf == 1) {
        if (feature_mask & 1) *c |= bit_SSSE3;
        if (feature_mask & 2) *c |= bit_SSE4_1;
    } else if (leaf == 7 && (feature_mask & 4)) *b |= bit_SHA;
    return 1;
}
static int test_cpuid_count(unsigned leaf, unsigned subleaf, unsigned* a,
                            unsigned* b, unsigned* c, unsigned* d) {
    assert(subleaf == 0);
    return test_cpuid(leaf, a, b, c, d);
}
#define __get_cpuid test_cpuid
#define __get_cpuid_count test_cpuid_count
#endif

#include "module/jash_crypto.c"

static void check_abc(void) {
    unsigned char digest[32];
    char hex[65];
    jash_sha256("abc", 3, digest);
    jash_sha256_hex(digest, hex);
    assert(!strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
}

static int start_workers;
static void* worker(void* unused) {
    (void)unused;
    while (!__atomic_load_n(&start_workers, __ATOMIC_ACQUIRE)) sched_yield();
    for (int i = 0; i < 500; i++) check_abc();
    return NULL;
}

int main(void) {
#if defined(JASH_SHA256_HAVE_SHA_NI)
    int native = host_has_sha() != 0;
    for (feature_mask = 0; feature_mask < 8; feature_mask++) {
        for (unsigned leaf_index = 0; leaf_index < 3; leaf_index++) {
            missing_leaf = leaf_index == 0 ? 0 : leaf_index == 1 ? 1 : 7;
            int eligible = feature_mask == 7 && missing_leaf == 0;
            assert(sha256_ni_cpu() == eligible);
            if (!eligible) {
                __atomic_store_n(&sha256_backend_mode, 0, __ATOMIC_RELAXED);
                check_abc();
                assert(__atomic_load_n(&sha256_backend_mode, __ATOMIC_RELAXED) == 1);
                unsigned before = __atomic_load_n(&queries, __ATOMIC_RELAXED);
                check_abc();
                assert(__atomic_load_n(&queries, __ATOMIC_RELAXED) == before);
            }
        }
    }
    missing_leaf = 0;
    feature_mask = native ? 7 : 0;
    if (native) {
        unsigned char block[64 + 15];
        for (unsigned seed = 0; seed < 256; seed++) {
            uint32_t random = seed + 0x12345678u;
            for (unsigned i = 0; i < sizeof block; i++) {
                random ^= random << 13;
                random ^= random >> 17;
                random ^= random << 5;
                block[i] = (unsigned char)random;
            }
            for (unsigned offset = 0; offset < 16; offset++) {
                jash_sha256_ctx_t scalar = {0}, accelerated;
                for (unsigned i = 0; i < 8; i++) scalar.state[i] = (seed + i + 1) * 0x9e3779b9u;
                memset(scalar.data, 0xa5, sizeof scalar.data);
                accelerated = scalar;
                sha256_transform_scalar(&scalar, block + offset);
                sha256_transform_ni(&accelerated, block + offset);
                assert(!memcmp(scalar.state, accelerated.state, sizeof scalar.state));
                assert(!memcmp(scalar.data, accelerated.data, sizeof scalar.data));
                assert(scalar.bitlen == accelerated.bitlen && scalar.datalen == accelerated.datalen);
            }
        }
        puts("PASS: 4096 SHA-NI/scalar compression comparisons");
    } else puts("SKIP: native SHA-NI unavailable; runtime fallback exercised");
    __atomic_store_n(&sha256_backend_mode, 0, __ATOMIC_RELAXED);
#else
    puts("PASS: portable-only build");
#endif
    pthread_t threads[8];
    for (unsigned i = 0; i < 8; i++) assert(pthread_create(&threads[i], NULL, worker, NULL) == 0);
    __atomic_store_n(&start_workers, 1, __ATOMIC_RELEASE);
    for (unsigned i = 0; i < 8; i++) assert(pthread_join(threads[i], NULL) == 0);
#if defined(JASH_SHA256_HAVE_SHA_NI)
    assert(__atomic_load_n(&sha256_backend_mode, __ATOMIC_RELAXED) == (native ? 2 : 1));
#endif
    puts("PASS: concurrent first use and independent hash contexts");
    return 0;
}
