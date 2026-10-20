#!/usr/bin/env python3
"""Check the backup hash primitive against independent SHA-256/HMAC oracles."""
import hashlib
import hmac
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
#include <module/jash_crypto.h>
#include <stdio.h>
#include <stdlib.h>
static unsigned char storage[1024 * 1024 + 16];
int main(int argc, char** argv) {
    if (argc != 4) return 2;
    int offset = atoi(argv[3]);
    if (offset < 0 || offset > 15) return 2;
    unsigned char* data = storage + offset;
    const size_t cap = 1024 * 1024 + 1;
    size_t length = fread(data, 1, cap, stdin);
    if (ferror(stdin) || length == cap) return 2;
    unsigned char digest[32];
    int step = atoi(argv[2]);
    if (argv[1][0] == 'h') {
        unsigned char key[200];
        if (step < 0 || step > 200) return 2;
        for (int i = 0; i < step; i++) key[i] = (unsigned char)(i * 13 + 7);
        jash_hmac_sha256(key, (size_t)step, data, length, digest);
    } else if (!step) jash_sha256(data, length, digest);
    else {
        jash_sha256_ctx_t ctx;
        jash_sha256_init(&ctx);
        size_t pos = 0, iteration = 0;
        jash_sha256_update(&ctx, NULL, 0);
        while (pos < length) {
            size_t part = step > 0 ? (size_t)step : (iteration++ * 37) % 127 + 1;
            if (part > length - pos) part = length - pos;
            jash_sha256_update(&ctx, data + pos, part);
            jash_sha256_update(&ctx, NULL, 0);
            pos += part;
        }
        jash_sha256_final(&ctx, digest);
    }
    char hex[65];
    jash_sha256_hex(digest, hex);
    puts(hex);
    return 0;
}
'''


class StreamingHash(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        tmp = tempfile.TemporaryDirectory(prefix="jash-sha-streaming-")
        cls.addClassCleanup(tmp.cleanup)
        cls.binaries = {}
        flags = ["-std=gnu11", "-O2", "-I" + str(ROOT / "c")]
        if os.environ.get("SANITIZE") == "1":
            flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
        for backend, defines in (("auto", []), ("portable", ["-DJASH_SHA256_PORTABLE_ONLY"])):
            binary = Path(tmp.name) / ("hash-" + backend)
            command = [os.environ.get("CC", "cc"), *flags, *defines, "-x", "c", "-",
                       str(ROOT / "c/module/jash_crypto.c"), str(ROOT / "c/module/blake3.c"),
                       "-o", str(binary)]
            subprocess.run(command, input=HARNESS.encode(), check=True, capture_output=True, timeout=60)
            cls.binaries[backend] = binary

    def digest(self, data, mode, step, offset=0):
        results = []
        for backend, binary in self.binaries.items():
            proc = subprocess.run([str(binary), mode, str(step), str(offset)], input=data,
                                  capture_output=True, timeout=15)
            self.assertEqual((proc.returncode, proc.stderr), (0, b""), backend)
            results.append(proc.stdout.decode().strip())
        self.assertEqual(results[0], results[1], "runtime-selected and portable digests differ")
        return results[0]

    def test_unaligned_inputs(self):
        for offset in range(16):
            for length in (64, 65, 129, 4096):
                data = bytes((i * 29 + 7) % 256 for i in range(length))
                with self.subTest(offset=offset, length=length):
                    self.assertEqual(self.digest(data, "sha", 0, offset), hashlib.sha256(data).hexdigest())

    def test_sha_boundaries_and_chunk_partitions(self):
        for length in (0, 1, 55, 56, 63, 64, 65, 127, 128, 129, 4096, 65535, 65536, 65537):
            data = (bytes(range(256)) * ((length + 255) // 256))[:length]
            for step in (0, 1, 7, 63, 64, 65, 8192, -1):
                with self.subTest(length=length, step=step):
                    self.assertEqual(self.digest(data, "sha", step), hashlib.sha256(data).hexdigest())
        data = b"a" * 1000000
        self.assertEqual(self.digest(data, "sha", 8192), hashlib.sha256(data).hexdigest())

    def test_hmac_key_and_message_boundaries(self):
        for key_length in (0, 1, 63, 64, 65, 200):
            key = bytes((i * 13 + 7) % 256 for i in range(key_length))
            for length in (0, 1, 55, 56, 64, 65, 4096):
                data = (b"payload\0" * ((length + 7) // 8))[:length]
                with self.subTest(key_length=key_length, length=length):
                    self.assertEqual(self.digest(data, "hmac", key_length),
                                     hmac.new(key, data, hashlib.sha256).hexdigest())


if __name__ == "__main__":
    unittest.main()
