// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — ADR-0003 S0: SHA-256 implementation
//
// Adapted from the public-domain algorithm (FIPS 180-4).
// No heap allocation, no external dependencies.

#include <module/jash_crypto.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/file.h>

/* Keep the baseline ISA and portable builds independent of x86 SHA support.
 * SHA/SSE instructions occur only in the target-specific function below. */
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__)) && \
    !defined(JASH_TARGET_BEYOND) && !defined(JASH_SHA256_PORTABLE_ONLY)
#define JASH_SHA256_HAVE_SHA_NI 1
#include <cpuid.h>
#include <immintrin.h>
#endif

/* ── SHA-256 constants ─────────────────────────────────────────────── */

static const uint32_t K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,
    0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
    0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,
    0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,
    0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
    0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,
    0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,
    0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
    0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2,
};

#define ROTR(x,n) (((x) >> (n)) | ((x) << (32-(n))))
#define CH(x,y,z) (((x)&(y))^(~(x)&(z)))
#define MAJ(x,y,z)(((x)&(y))^((x)&(z))^((y)&(z)))
#define EP0(x)    (ROTR(x,2)^ROTR(x,13)^ROTR(x,22))
#define EP1(x)    (ROTR(x,6)^ROTR(x,11)^ROTR(x,25))
#define SIG0(x)   (ROTR(x,7)^ROTR(x,18)^((x)>>3))
#define SIG1(x)   (ROTR(x,17)^ROTR(x,19)^((x)>>10))

static void sha256_schedule(const uint8_t data[64], uint32_t m[64])
{
    uint32_t i,j;
    for (i=0,j=0; i<16; i++,j+=4)
        m[i] = ((uint32_t)data[j]<<24)|((uint32_t)data[j+1]<<16)|
               ((uint32_t)data[j+2]<<8)|(uint32_t)data[j+3];
    for (; i<64; i++)
        m[i] = SIG1(m[i-2])+m[i-7]+SIG0(m[i-15])+m[i-16];
}

static void sha256_transform_scalar(jash_sha256_ctx_t* ctx, const uint8_t data[64])
{
    uint32_t a,b,c,d,e,f,g,h,t1,t2,m[64];
    sha256_schedule(data, m);

    a=ctx->state[0]; b=ctx->state[1]; c=ctx->state[2]; d=ctx->state[3];
    e=ctx->state[4]; f=ctx->state[5]; g=ctx->state[6]; h=ctx->state[7];

    for (unsigned i=0; i<64; i++) {
        t1 = h+EP1(e)+CH(e,f,g)+K[i]+m[i];
        t2 = EP0(a)+MAJ(a,b,c);
        h=g; g=f; f=e; e=d+t1;
        d=c; c=b; b=a; a=t1+t2;
    }

    ctx->state[0]+=a; ctx->state[1]+=b; ctx->state[2]+=c; ctx->state[3]+=d;
    ctx->state[4]+=e; ctx->state[5]+=f; ctx->state[6]+=g; ctx->state[7]+=h;
}

#if defined(JASH_SHA256_HAVE_SHA_NI)
static int sha256_ni_cpu(void)
{
    unsigned a, b, c, d;
    if (!__get_cpuid(1, &a, &b, &c, &d)) return 0;
    /* The state-layout shuffles below need both SSSE3 and SSE4.1. */
    if ((c & (bit_SSSE3 | bit_SSE4_1)) != (bit_SSSE3 | bit_SSE4_1)) return 0;
    if (!__get_cpuid_count(7, 0, &a, &b, &c, &d)) return 0;
    return (b & bit_SHA) != 0;
}

/* 0 unknown, 1 portable, 2 SHA-NI. Concurrent first users may probe twice,
 * but never race on an ordinary static variable or publish a partial state. */
static int sha256_backend_mode;
static int sha256_use_ni(void)
{
    int mode = __atomic_load_n(&sha256_backend_mode, __ATOMIC_RELAXED);
    if (!mode) {
        mode = sha256_ni_cpu() ? 2 : 1;
        __atomic_store_n(&sha256_backend_mode, mode, __ATOMIC_RELAXED);
    }
    return mode == 2;
}

__attribute__((target("sha,ssse3,sse4.1"), noinline))
static void sha256_transform_ni(jash_sha256_ctx_t* ctx, const uint8_t data[64])
{
    const __m128i byte_order = _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4,
                                            11, 10, 9, 8, 15, 14, 13, 12);
    __m128i words[4];
    for (unsigned i = 0; i < 4; i++)
        words[i] = _mm_shuffle_epi8(
            _mm_loadu_si128((const __m128i*)(const void*)(data + 16 * i)), byte_order);
    __m128i abcd = _mm_loadu_si128((const __m128i*)(const void*)ctx->state);
    __m128i efgh = _mm_loadu_si128((const __m128i*)(const void*)(ctx->state + 4));
    __m128i tmp = _mm_shuffle_epi32(abcd, 0xb1);
    efgh = _mm_shuffle_epi32(efgh, 0x1b);
    /* SHA256RNDS2 consumes ABEF/CDGH, high lane first. */
    __m128i abef = _mm_alignr_epi8(tmp, efgh, 8);
    __m128i cdgh = _mm_blend_epi16(efgh, tmp, 0xf0);
    const __m128i saved_abef = abef, saved_cdgh = cdgh;
    for (unsigned i = 0; i < 16; i++) {
        unsigned slot = i & 3;
        if (i >= 4) {
            /* Four words at a time: W[t-16] + sigma0(W[t-15]) +
             * W[t-7] + sigma1(W[t-2]), retaining only the last 16 words. */
            __m128i next = _mm_sha256msg1_epu32(words[slot], words[(slot + 1) & 3]);
            next = _mm_add_epi32(next,
                _mm_alignr_epi8(words[(slot + 3) & 3], words[(slot + 2) & 3], 4));
            words[slot] = _mm_sha256msg2_epu32(next, words[(slot + 3) & 3]);
        }
        __m128i msg = _mm_add_epi32(words[slot],
            _mm_loadu_si128((const __m128i*)(const void*)(K + 4 * i)));
        cdgh = _mm_sha256rnds2_epu32(cdgh, abef, msg);
        msg = _mm_shuffle_epi32(msg, 0x0e);
        abef = _mm_sha256rnds2_epu32(abef, cdgh, msg);
    }
    abef = _mm_add_epi32(abef, saved_abef);
    cdgh = _mm_add_epi32(cdgh, saved_cdgh);
    tmp = _mm_shuffle_epi32(abef, 0x1b);
    cdgh = _mm_shuffle_epi32(cdgh, 0xb1);
    abcd = _mm_blend_epi16(tmp, cdgh, 0xf0);
    efgh = _mm_alignr_epi8(cdgh, tmp, 8);
    _mm_storeu_si128((__m128i*)(void*)ctx->state, abcd);
    _mm_storeu_si128((__m128i*)(void*)(ctx->state + 4), efgh);
}
#endif

static void sha256_transform(jash_sha256_ctx_t* ctx, const uint8_t data[64])
{
#if defined(JASH_SHA256_HAVE_SHA_NI)
    if (sha256_use_ni()) { sha256_transform_ni(ctx, data); return; }
#endif
    sha256_transform_scalar(ctx, data);
}

void jash_sha256_init(jash_sha256_ctx_t* ctx)
{
    ctx->datalen = 0;
    ctx->bitlen  = 0;
    ctx->state[0]=0x6a09e667; ctx->state[1]=0xbb67ae85;
    ctx->state[2]=0x3c6ef372; ctx->state[3]=0xa54ff53a;
    ctx->state[4]=0x510e527f; ctx->state[5]=0x9b05688c;
    ctx->state[6]=0x1f83d9ab; ctx->state[7]=0x5be0cd19;
}

void jash_sha256_update(jash_sha256_ctx_t* ctx, const uint8_t* data, size_t len)
{
    if (!len) return;
    if (ctx->datalen) {
        size_t part = 64 - ctx->datalen;
        if (part > len) part = len;
        memcpy(ctx->data + ctx->datalen, data, part);
        ctx->datalen += (uint32_t)part;
        data += part;
        len -= part;
        if (ctx->datalen == 64) {
            sha256_transform(ctx, ctx->data);
            ctx->bitlen += 512;
            ctx->datalen = 0;
        }
    }
    /* Complete source blocks need no bytewise staging. The transform reads
     * bytes, so this also accepts unaligned input and arbitrary partitions. */
    while (len >= 64) {
        sha256_transform(ctx, data);
        ctx->bitlen += 512;
        data += 64;
        len -= 64;
    }
    if (len) {
        memcpy(ctx->data, data, len);
        ctx->datalen = (uint32_t)len;
    }
}

void jash_sha256_final(jash_sha256_ctx_t* ctx, uint8_t digest[JASH_SHA256_DIGEST_LEN])
{
    uint32_t i = ctx->datalen;
    if (ctx->datalen < 56) {
        ctx->data[i++] = 0x80;
        while (i<56) ctx->data[i++]=0;
    } else {
        ctx->data[i++] = 0x80;
        while (i<64) ctx->data[i++]=0;
        sha256_transform(ctx, ctx->data);
        memset(ctx->data, 0, 56);
    }
    ctx->bitlen += (uint64_t)ctx->datalen*8;
    ctx->data[63]=(uint8_t)(ctx->bitlen);
    ctx->data[62]=(uint8_t)(ctx->bitlen>>8);
    ctx->data[61]=(uint8_t)(ctx->bitlen>>16);
    ctx->data[60]=(uint8_t)(ctx->bitlen>>24);
    ctx->data[59]=(uint8_t)(ctx->bitlen>>32);
    ctx->data[58]=(uint8_t)(ctx->bitlen>>40);
    ctx->data[57]=(uint8_t)(ctx->bitlen>>48);
    ctx->data[56]=(uint8_t)(ctx->bitlen>>56);
    sha256_transform(ctx, ctx->data);

    for (i=0; i<4; i++) {
        digest[i]   =(uint8_t)(ctx->state[0]>>(24-i*8));
        digest[i+4] =(uint8_t)(ctx->state[1]>>(24-i*8));
        digest[i+8] =(uint8_t)(ctx->state[2]>>(24-i*8));
        digest[i+12]=(uint8_t)(ctx->state[3]>>(24-i*8));
        digest[i+16]=(uint8_t)(ctx->state[4]>>(24-i*8));
        digest[i+20]=(uint8_t)(ctx->state[5]>>(24-i*8));
        digest[i+24]=(uint8_t)(ctx->state[6]>>(24-i*8));
        digest[i+28]=(uint8_t)(ctx->state[7]>>(24-i*8));
    }
}

void jash_sha256(const void* data, size_t len, uint8_t digest[JASH_SHA256_DIGEST_LEN])
{
    jash_sha256_ctx_t ctx;
    jash_sha256_init(&ctx);
    jash_sha256_update(&ctx, (const uint8_t*)data, len);
    jash_sha256_final(&ctx, digest);
}

void jash_sha256_hex(const uint8_t digest[JASH_SHA256_DIGEST_LEN],
                     char out[JASH_SHA256_HEX_LEN])
{
    static const char hex[] = "0123456789abcdef";
    for (int i=0; i<JASH_SHA256_DIGEST_LEN; i++) {
        out[i*2]   = hex[digest[i]>>4];
        out[i*2+1] = hex[digest[i]&0xf];
    }
    out[64] = '\0';
}

/* ── HMAC-SHA256 ───────────────────────────────────────────────────── */

void jash_hmac_sha256(const uint8_t* key, size_t key_len,
                      const uint8_t* data, size_t data_len,
                      uint8_t mac[JASH_SHA256_DIGEST_LEN])
{
    uint8_t tkey[JASH_SHA256_DIGEST_LEN];
    if (key_len > 64) {
        jash_sha256(key, key_len, tkey);
        key = tkey; key_len = JASH_SHA256_DIGEST_LEN;
    }

    uint8_t k_ipad[64], k_opad[64];
    memset(k_ipad, 0x36, 64);
    memset(k_opad, 0x5c, 64);
    for (size_t i = 0; i < key_len; i++) {
        k_ipad[i] ^= key[i];
        k_opad[i] ^= key[i];
    }

    uint8_t inner[JASH_SHA256_DIGEST_LEN];
    jash_sha256_ctx_t ctx;
    jash_sha256_init(&ctx);
    jash_sha256_update(&ctx, k_ipad, 64);
    jash_sha256_update(&ctx, data, data_len);
    jash_sha256_final(&ctx, inner);

    jash_sha256_init(&ctx);
    jash_sha256_update(&ctx, k_opad, 64);
    jash_sha256_update(&ctx, inner, JASH_SHA256_DIGEST_LEN);
    jash_sha256_final(&ctx, mac);
}

/* ── Session key ───────────────────────────────────────────────────── */

int jash_session_key_load(uint8_t key[JASH_SHA256_DIGEST_LEN])
{
    const char* home = getenv("HOME");
    if (!home || !*home) return -1;

    char dir[512];
    int dl = snprintf(dir, sizeof dir, "%s/.jash", home);
    if (dl <= 0 || dl >= (int)sizeof dir) return -1;

    struct stat path_st, dir_st;
    if (lstat(dir, &path_st) != 0) {
        if (errno != ENOENT) return -1;
        if (mkdir(dir, 0700) != 0 && errno != EEXIST) return -1;
        if (lstat(dir, &path_st) != 0) return -1;
    }
    if (!S_ISDIR(path_st.st_mode) || path_st.st_uid != geteuid() ||
        (path_st.st_mode & 077) != 0) return -1;

    int dirfd = open(dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (dirfd < 0) return -1;
    if (fstat(dirfd, &dir_st) != 0 || !S_ISDIR(dir_st.st_mode) ||
        dir_st.st_dev != path_st.st_dev || dir_st.st_ino != path_st.st_ino ||
        dir_st.st_uid != geteuid() || (dir_st.st_mode & 077) != 0) {
        close(dirfd);
        return -1;
    }

    int lockfd = openat(dirfd, ".session.key.lock",
                        O_RDWR | O_CREAT | O_NOFOLLOW, 0600);
    if (lockfd < 0) { close(dirfd); return -1; }
    struct stat lock_st;
    if (fstat(lockfd, &lock_st) != 0 || !S_ISREG(lock_st.st_mode) ||
        lock_st.st_uid != geteuid() || (lock_st.st_mode & 077) != 0 ||
        flock(lockfd, LOCK_EX) != 0) {
        close(lockfd);
        close(dirfd);
        return -1;
    }

    int rc = -1;
    int publish_races = 0;
load_key:
    ;
    int kfd = openat(dirfd, "session.key", O_RDONLY | O_NOFOLLOW);
    if (kfd >= 0) {
        struct stat kst;
        char hex[64];
        size_t got = 0;
        if (fstat(kfd, &kst) != 0 || !S_ISREG(kst.st_mode) ||
            kst.st_uid != geteuid() || (kst.st_mode & 077) != 0 ||
            kst.st_size != (off_t)sizeof hex) goto key_done;
        while (got < sizeof hex) {
            ssize_t n = read(kfd, hex + got, sizeof hex - got);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) goto key_done;
            got += (size_t)n;
        }
        for (int i = 0; i < JASH_SHA256_DIGEST_LEN; i++) {
            int hi = hex[i * 2], lo = hex[i * 2 + 1];
            hi = hi >= '0' && hi <= '9' ? hi - '0' :
                 hi >= 'a' && hi <= 'f' ? hi - 'a' + 10 :
                 hi >= 'A' && hi <= 'F' ? hi - 'A' + 10 : -1;
            lo = lo >= '0' && lo <= '9' ? lo - '0' :
                 lo >= 'a' && lo <= 'f' ? lo - 'a' + 10 :
                 lo >= 'A' && lo <= 'F' ? lo - 'A' + 10 : -1;
            if (hi < 0 || lo < 0) goto key_done;
            key[i] = (uint8_t)((hi << 4) | lo);
        }
        rc = 0;
key_done:
        close(kfd);
        goto done;
    }
    if (errno != ENOENT) goto done;

    FILE* rng = fopen("/dev/urandom", "rb");
    if (!rng) goto done;
    size_t random_n = fread(key, 1, JASH_SHA256_DIGEST_LEN, rng);
    fclose(rng);
    if (random_n != JASH_SHA256_DIGEST_LEN) goto done;

    char tmpname[96];
    snprintf(tmpname, sizeof tmpname, ".session.key.tmp.%ld.%02x%02x%02x%02x",
             (long)getpid(), key[0], key[1], key[2], key[3]);
    int wfd = openat(dirfd, tmpname,
                     O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (wfd < 0) goto done;
    char hex[65];
    jash_sha256_hex(key, hex);
    size_t off = 0;
    while (off < 64) {
        ssize_t n = write(wfd, hex + off, 64 - off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        off += (size_t)n;
    }
    int persisted = off == 64 && fsync(wfd) == 0;
    if (close(wfd) != 0) persisted = 0;
    if (!persisted) {
        unlinkat(dirfd, tmpname, 0);
        goto done;
    }

    if (linkat(dirfd, tmpname, dirfd, "session.key", 0) != 0) {
        int publish_errno = errno;
        unlinkat(dirfd, tmpname, 0);
        if (publish_errno == EEXIST && publish_races++ == 0) goto load_key;
        goto done;
    }
    unlinkat(dirfd, tmpname, 0);
    if (fsync(dirfd) == 0) rc = 0;

done:
    flock(lockfd, LOCK_UN);
    close(lockfd);
    close(dirfd);
    return rc;
}

void jash_sha256_chain(const uint8_t prev[JASH_SHA256_DIGEST_LEN],
                       const void* data, size_t len,
                       uint8_t out[JASH_SHA256_DIGEST_LEN])
{
    jash_sha256_ctx_t ctx;
    jash_sha256_init(&ctx);
    jash_sha256_update(&ctx, prev, JASH_SHA256_DIGEST_LEN);
    jash_sha256_update(&ctx, (const uint8_t*)data, len);
    jash_sha256_final(&ctx, out);
}

/* ── BLAKE3 wrappers (S7) ──────────────────────────────────────────── */

void jash_blake3(const void* data, size_t len, uint8_t out[32])
{
    blake3_hash(data, (uint64_t)len, out);
}

void jash_blake3_hex(const uint8_t digest[32], char out[JASH_BLAKE3_HEX_LEN])
{
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[i*2]   = hex[digest[i] >> 4];
        out[i*2+1] = hex[digest[i] & 0xf];
    }
    out[64] = '\0';
}

void jash_blake3_keyed(const uint8_t key[32], const void* data, size_t len,
                       uint8_t out[32])
{
    struct blake3 h;
    blake3_keyed_init(&h, key);
    blake3_update(&h, data, (uint64_t)len);
    blake3_final(&h, out, 32);
}

void jash_blake3_chain(const uint8_t prev[32],
                       const void* payload, size_t len,
                       uint8_t out[32])
{
    struct blake3 h;
    blake3_init(&h);
    blake3_update(&h, prev, 32);
    blake3_update(&h, payload, (uint64_t)len);
    blake3_final(&h, out, 32);
}

void jash_blake3_ratchet(uint8_t key[32])
{
    struct blake3 h;
    uint8_t next[32];
    blake3_init(&h);
    blake3_update(&h, key, 32);
    blake3_update(&h, "r", 1);
    blake3_final(&h, next, 32);
    memcpy(key, next, 32);
    memset(next, 0, 32);
}
