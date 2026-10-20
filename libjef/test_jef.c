// SPDX-License-Identifier: LicenseRef-SPL-1.0
// libjef standalone unit test — exercises the sovereign attestation crypto
// with NO jash present (proves libjef is independently consumable).
//
// Build+run: cmake --build <build> --target jef_test && <build>/jef_test

#include <jef.h>
#include <stdio.h>
#include <string.h>

static int pass = 0, fail = 0;
static void ok(const char* m){ pass++; printf("  ok  %s\n", m); }
static void no(const char* m){ fail++; printf("  FAIL %s\n", m); }

int main(void) {
    printf("== libjef %s standalone crypto test ==\n", JEF_VERSION_STRING);

    /* 1. SHA-256 known-answer test: SHA256("abc"). */
    {
        uint8_t d[32]; char hex[JASH_SHA256_HEX_LEN];
        jash_sha256("abc", 3, d);
        jash_sha256_hex(d, hex);
        const char* want = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
        if (strcmp(hex, want) == 0) ok("SHA-256(\"abc\") matches known vector");
        else { no("SHA-256 KAT"); printf("    got %s\n    want %s\n", hex, want); }
    }

    /* 2. HMAC-SHA256: deterministic + key-sensitive. */
    {
        uint8_t k1[32], k2[32], m1[32], m1b[32], m2[32];
        memset(k1, 0x11, 32); memset(k2, 0x22, 32);
        jash_hmac_sha256(k1, 32, (const uint8_t*)"data", 4, m1);
        jash_hmac_sha256(k1, 32, (const uint8_t*)"data", 4, m1b);
        jash_hmac_sha256(k2, 32, (const uint8_t*)"data", 4, m2);
        if (memcmp(m1, m1b, 32) == 0) ok("HMAC-SHA256 deterministic");
        else no("HMAC determinism");
        if (memcmp(m1, m2, 32) != 0) ok("HMAC key-sensitive");
        else no("HMAC key sensitivity");
    }

    /* 3. BLAKE3: deterministic, and keyed differs from unkeyed. */
    {
        uint8_t a[32], b[32], key[32], kd[32];
        jash_blake3("payload", 7, a);
        jash_blake3("payload", 7, b);
        memset(key, 0x42, 32);
        jash_blake3_keyed(key, "payload", 7, kd);
        if (memcmp(a, b, 32) == 0) ok("BLAKE3 deterministic");
        else no("BLAKE3 determinism");
        if (memcmp(a, kd, 32) != 0) ok("BLAKE3 keyed differs from plain");
        else no("BLAKE3 keyed");
    }

    /* 4. Hash chain: h1 = chain(0, p0); h2 = chain(h1, p1); h1 != h2, and the
     *    chain is reproducible (tamper-evidence foundation). */
    {
        uint8_t zero[32] = {0}, h1[32], h2[32], h1r[32];
        jash_blake3_chain(zero, "cmd0", 4, h1);
        jash_blake3_chain(h1,  "cmd1", 4, h2);
        jash_blake3_chain(zero, "cmd0", 4, h1r);
        if (memcmp(h1, h2, 32) != 0) ok("chain advances (h1 != h2)");
        else no("chain advance");
        if (memcmp(h1, h1r, 32) == 0) ok("chain reproducible (recompute matches)");
        else no("chain reproducibility");
        /* a different command at step 0 must yield a different tip */
        uint8_t h1x[32];
        jash_blake3_chain(zero, "cmdX", 4, h1x);
        if (memcmp(h1, h1x, 32) != 0) ok("chain detects altered command");
        else no("chain tamper sensitivity");
    }

    /* 5. Forward-secure ratchet: key changes after advance and is one-way. */
    {
        uint8_t k[32], k0[32];
        memset(k, 0x55, 32); memcpy(k0, k, 32);
        jash_blake3_ratchet(k);
        if (memcmp(k, k0, 32) != 0) ok("ratchet advances key");
        else no("ratchet advance");
    }

    /* 6. Ed25519 (RFC 8032) — sovereign asymmetric signatures (W4). Self-
     *    consistency suite: deterministic keygen, sign→verify round-trip, and
     *    rejection of tampered message / signature / wrong key. Proves the
     *    vendored public-domain TweetNaCl port signs & verifies correctly. */
    {
        uint8_t seedA[32], seedB[32];
        memset(seedA, 0xA5, 32); memset(seedB, 0x5A, 32);
        uint8_t pkA[ED25519_PUBKEY], skA[ED25519_SECKEY];
        uint8_t pkA2[ED25519_PUBKEY], skA2[ED25519_SECKEY];
        uint8_t pkB[ED25519_PUBKEY], skB[ED25519_SECKEY];
        ed25519_keypair_from_seed(seedA, pkA,  skA);
        ed25519_keypair_from_seed(seedA, pkA2, skA2);
        ed25519_keypair_from_seed(seedB, pkB,  skB);

        if (memcmp(pkA, pkA2, ED25519_PUBKEY) == 0) ok("Ed25519 keygen deterministic");
        else no("Ed25519 keygen determinism");
        if (memcmp(pkA, pkB, ED25519_PUBKEY) != 0) ok("Ed25519 distinct seeds -> distinct pubkeys");
        else no("Ed25519 seed independence");

        const char* msg = "jashproof/1 chain_tip: deadbeef...";
        uint32_t mlen = (uint32_t)strlen(msg);
        uint8_t sig[ED25519_SIG];
        ed25519_sign(sig, (const uint8_t*)msg, mlen, skA);

        if (ed25519_verify(sig, (const uint8_t*)msg, mlen, pkA) == 0)
            ok("Ed25519 sign->verify round-trip (valid)");
        else no("Ed25519 round-trip");

        /* tampered message rejected */
        char bad[64]; memcpy(bad, msg, mlen+1); bad[0] ^= 0x01;
        if (ed25519_verify(sig, (const uint8_t*)bad, mlen, pkA) != 0)
            ok("Ed25519 rejects tampered message");
        else no("Ed25519 message tamper");

        /* tampered signature rejected */
        uint8_t sigbad[ED25519_SIG]; memcpy(sigbad, sig, ED25519_SIG); sigbad[10] ^= 0x01;
        if (ed25519_verify(sigbad, (const uint8_t*)msg, mlen, pkA) != 0)
            ok("Ed25519 rejects tampered signature");
        else no("Ed25519 sig tamper");

        /* wrong public key rejected (third-party-verifiable property) */
        if (ed25519_verify(sig, (const uint8_t*)msg, mlen, pkB) != 0)
            ok("Ed25519 rejects wrong public key");
        else no("Ed25519 wrong key");
    }

    printf("== libjef: %d passed, %d failed ==\n", pass, fail);
    return fail == 0 ? 0 : 1;
}
