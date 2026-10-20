/* Standalone carry-UB regression. Build outside the shared build tree:
 * cc -std=c11 -O0 -g -fsanitize=address,undefined -fno-sanitize-recover=all
 *   -fno-omit-frame-pointer -no-pie -Ilibjef libjef/ed25519.c
 *   tests/test_ed25519_ubsan.c -o /tmp/opencode/test_ed25519_ubsan
 * Repeat at -O2. No sanitizer suppressions. --dump emits the deterministic
 * corpus's public keys/signatures for comparison with an original plain build.
 */
#include "ed25519.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void unhex(uint8_t *out, const char *hex) {
    while (*hex) {
        unsigned byte;
        assert(sscanf(hex, "%2x", &byte) == 1);
        *out++ = (uint8_t)byte;
        hex += 2;
    }
}

static void dump_hex(const uint8_t *data, unsigned len) {
    for (unsigned i = 0; i < len; ++i) printf("%02x", data[i]);
    putchar('\n');
}

int main(int argc, char **argv) {
    static const uint8_t order[32] = {
        0xed,0xd3,0xf5,0x5c,0x1a,0x63,0x12,0x58,0xd6,0x9c,0xf7,0xa2,0xde,0xf9,0xde,0x14,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0x10
    };
    /* RFC 8032 section 7.1, TEST 1, TEST 2 and TEST 3 (not self-oracles). */
    static const struct { const char *seed, *pk, *msg, *sig; } vectors[] = {
        {
            "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
            "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a",
            "",
            "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
            "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"
        },
        {
            "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
            "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c",
            "72",
            "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da"
            "085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00"
        },
        {
            "c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7",
            "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025",
            "af82",
            "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac"
            "18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a"
        }
    };
    const int dump = argc == 2 && strcmp(argv[1], "--dump") == 0;
    uint8_t seed[32], pk[32], sk[64], sig[64], expected_pk[32], expected_sig[64];
    uint8_t msg[ED25519_MAX_MSG + 1] = {0}, bad[64], wrong_pk[32], wrong_sk[64];
    for (unsigned v = 0; v < sizeof vectors / sizeof vectors[0]; ++v) {
        uint32_t len = (uint32_t)strlen(vectors[v].msg) / 2;
        unhex(seed, vectors[v].seed);
        unhex(expected_pk, vectors[v].pk);
        unhex(expected_sig, vectors[v].sig);
        unhex(msg, vectors[v].msg);
        ed25519_keypair_from_seed(seed, pk, sk);
        assert(memcmp(pk, expected_pk, 32) == 0);
        assert(memcmp(sk, seed, 32) == 0 && memcmp(sk + 32, pk, 32) == 0);
        assert(ed25519_sign(sig, msg, len, sk) == 0);
        assert(memcmp(sig, expected_sig, 64) == 0);
        assert(ed25519_verify(expected_sig, msg, len, expected_pk) == 0);
        /* Corrupt both R and S, separately, against the published key. */
        for (unsigned i = 0; i < 64; ++i) {
            memcpy(bad, expected_sig, 64);
            bad[i] ^= 1;
            assert(ed25519_verify(bad, msg, len, expected_pk) == -1);
        }
        /* RFC 8032 requires canonical S. The group equation alone accepts
         * S+L, which gives a second byte representation of the same signature. */
        memcpy(bad, expected_sig, 64);
        unsigned carry = 0;
        for (unsigned i = 0; i < 32; ++i) {
            unsigned sum = bad[32 + i] + order[i] + carry;
            bad[32 + i] = (uint8_t)sum;
            carry = sum >> 8;
        }
        assert(carry == 0);
        assert(ed25519_verify(bad, msg, len, expected_pk) == -1);
    }

    /* SHA-512 padding/block edges for both signing prefixes (32 and 64),
     * plus long and maximum messages, across extreme and mixed seeds. */
    static const uint32_t lengths[] = {
        0, 1, 2, 47, 48, 63, 64, 79, 80, 95, 96, 111, 112, 127, 128,
        255, 256, 1023, 1024, ED25519_MAX_MSG - 1, ED25519_MAX_MSG
    };
    memset(seed, 0x42, sizeof seed);
    ed25519_keypair_from_seed(seed, wrong_pk, wrong_sk);
    unsigned cases = 0;
    for (unsigned s = 0; s < 6; ++s) {
        for (unsigned i = 0; i < sizeof seed; ++i)
            seed[i] = s == 0 ? 0 : s == 1 ? 255 : s == 2 ? 0xaa :
                      s == 3 ? 0x55 : s == 4 ? (uint8_t)i : (uint8_t)(i * 73 + 19);
        ed25519_keypair_from_seed(seed, pk, sk);
        for (unsigned i = 0; i < sizeof msg; ++i)
            msg[i] = s == 0 ? 0 : s == 1 ? 255 : (uint8_t)(i * 29 + s * 71);
        for (unsigned n = 0; n < sizeof lengths / sizeof lengths[0]; ++n) {
            uint32_t len = lengths[n];
            assert(ed25519_sign(sig, msg, len, sk) == 0);
            assert(ed25519_verify(sig, msg, len, pk) == 0);
            if (dump) { dump_hex(pk, 32); dump_hex(sig, 64); }
            assert(ed25519_verify(sig, msg, len, wrong_pk) == -1);
            memcpy(bad, sig, 64);
            bad[(s + n) % 64] ^= 0x80;
            assert(ed25519_verify(bad, msg, len, pk) == -1);
            memset(bad, 0xff, 64);
            assert(ed25519_verify(bad, msg, len, pk) == -1);
            memset(bad, 0, 64);
            assert(ed25519_verify(bad, msg, len, pk) == -1);
            msg[0] ^= 1;
            assert(ed25519_verify(sig, msg, len ? len : 1, pk) == -1);
            msg[0] ^= 1;
            ++cases;
        }
    }
    memset(sig, 0xa5, sizeof sig);
    memcpy(bad, sig, sizeof sig);
    assert(ed25519_sign(sig, msg, ED25519_MAX_MSG + 1, sk) == -1);
    assert(memcmp(sig, bad, sizeof sig) == 0);
    assert(ed25519_verify(sig, msg, ED25519_MAX_MSG + 1, pk) == -1);
    printf("Ed25519: 3 RFC 8032 byte-exact vectors, 192 signature mutations, "
           "%u corpus cases and message limits passed\n", cases);
    return 0;
}
