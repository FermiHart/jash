#!/usr/bin/env python3
"""Isolated input-boundary regressions; no shared build or Python dependencies.

Run normally, or with JEF_SANITIZE=1 for ASan+UBSan at -O0 on ALL libjef
sources plus the RFC 8032 carry regression at -O2. Generated binaries and
fixtures use TMPDIR. Set JEF_TEST_DIR to retain them (including a standalone
jef_verify for adjacent suites). JEF_ADJACENT=1 also runs existing receipt-suite
Python bodies without their shared-build wrappers.
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
#include "jef.h"
#include "jef_verify.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

FILE *__real_fopen(const char *, const char *);
static const char *fault_data;
static size_t fault_left;
static const char *race_path, *race_replacement;
static ssize_t fail_read(void *cookie, char *buf, size_t n) {
    (void)cookie;
    if (!fault_left) { errno = EIO; return -1; }
    if (n > fault_left) n = fault_left;
    memcpy(buf, fault_data, n);
    fault_data += n; fault_left -= n;
    return (ssize_t)n;
}
FILE *__wrap_fopen(const char *path, const char *mode) {
    if (strcmp(path, "injected-read-error") == 0) {
        cookie_io_functions_t io = {.read = fail_read};
        return fopencookie(NULL, "r", io);
    }
    FILE *f = __real_fopen(path, mode);
    if (f && race_path && strcmp(path, race_path) == 0) {
        if (rename(race_replacement, race_path) != 0) return NULL;
        race_path = NULL;
    }
    return f;
}
int main(int argc, char **argv) {
    if (argc < 3) return 2;
    const char *mode = argv[1], *path = argv[2];
    unsigned long long entries = 999;
    char hex[129];
    if (!strcmp(mode, "chain") || !strcmp(mode, "readerror")) {
        char data[16384];
        if (!strcmp(mode, "readerror")) {
            FILE *f = __real_fopen(path, "rb");
            if (!f) return 2;
            fault_left = fread(data, 1, sizeof data, f);
            fclose(f); fault_data = data; path = "injected-read-error";
        }
        int rc = jef_verify_chain(path, &entries);
        printf("%d %llu\n", rc, entries);
    } else if (!strcmp(mode, "proof")) {
        printf("%d\n", jef_verify_proof(path));
    } else if (!strcmp(mode, "extract")) {
        int rc = jef_proof_pubkey_hex(path, hex, sizeof hex);
        printf("%d\n", rc);
        if (!rc) puts(hex);
    } else if (!strcmp(mode, "pinned")) {
        if (argc != 4) return 2;
        printf("%d\n", jef_verify_proof_pinned(path, argv[3]));
    } else if (!strcmp(mode, "pinnedrace")) {
        if (argc != 5) return 2;
        race_path = path;
        race_replacement = argv[4];
        printf("%d\n", jef_verify_proof_pinned(path, argv[3]));
    } else if (!strcmp(mode, "sign")) {
        uint8_t seed[32] = {1}, pk[32], sk[64], sig[64], data[4096];
        ed25519_keypair_from_seed(seed, pk, sk);
        FILE *f = __real_fopen(path, "rb");
        if (!f) return 2;
        size_t n = fread(data, 1, sizeof data, f);
        fclose(f);
        if (ed25519_sign(sig, data, (uint32_t)n, sk)) return 2;
        jash_blake3_hex(pk, hex); puts(hex);
        for (unsigned i = 0; i < 64; i++) printf("%02x", sig[i]);
        puts("");
    } else if (!strcmp(mode, "record")) {
        /* The producer's exact ts_be || rc_le || escaped-command payload. */
        size_t n = strlen(path);
        if (n > 4084 || argc != 5) return 2;
        uint64_t ts = strtoull(argv[3], NULL, 10);
        int rc = atoi(argv[4]);
        uint8_t payload[4096], prev[32] = {0}, hash[32];
        for (unsigned i = 0; i < 8; i++) payload[i] = ts >> (56 - 8*i);
        for (unsigned i = 0; i < 4; i++) payload[8+i] = (uint32_t)rc >> (8*i);
        memcpy(payload + 12, path, n);
        jash_blake3_chain(prev, payload, n + 12, hash);
        jash_blake3_hex(hash, hex);
        printf("0 %llu.%09llu %s tag=%064d rc=%d cmd=%s\n",
               (unsigned long long)(ts / 1000000000),
               (unsigned long long)(ts % 1000000000), hex, 0, rc, path);
    } else return 2;
    return 0;
}
'''


class InputBounds(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        retained = os.environ.get("JEF_TEST_DIR")
        cls.temp = None if retained else tempfile.TemporaryDirectory(prefix="libjef-input-")
        cls.work = Path(retained or cls.temp.name)
        cls.work.mkdir(parents=True, exist_ok=True)
        cls.driver_env = dict(os.environ)
        flags = ["-std=c11", "-D_GNU_SOURCE", "-O0", "-g", "-I" + str(ROOT / "c"),
                 "-I" + str(ROOT / "libjef")]
        if os.environ.get("JEF_SANITIZE") == "1":
            flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                      "-fno-sanitize-recover=all", "-no-pie"]
            cls.driver_env["ASAN_OPTIONS"] = "halt_on_error=1"
            cls.driver_env["UBSAN_OPTIONS"] = "halt_on_error=1"
        sources = [ROOT / p for p in ("c/module/jash_crypto.c", "c/module/blake3.c",
                                      "libjef/ed25519.c", "libjef/jef_verify.c")]
        cc = [os.environ.get("CC", "cc"), *flags, *map(str, sources)]
        cls.driver = cls.work / "bounds"
        subprocess.run([*cc, "-Wl,--wrap=fopen", "-x", "c", "-", "-o", str(cls.driver)],
                       input=HARNESS, text=True, check=True, capture_output=True)
        subprocess.run([*cc, str(ROOT / "libjef/jef_verify_main.c"),
                        "-o", str(cls.work / "jef_verify")], check=True,
                       capture_output=True)
        if os.environ.get("JEF_SANITIZE") == "1":
            vector_flags = ["-O2" if flag == "-O0" else flag for flag in flags]
            vector = cls.work / "ed25519-ubsan"
            subprocess.run([os.environ.get("CC", "cc"), *vector_flags,
                            str(ROOT / "libjef/ed25519.c"),
                            str(ROOT / "tests/test_ed25519_ubsan.c"), "-o", str(vector)],
                           check=True, capture_output=True)
            subprocess.run([str(vector)], check=True, capture_output=True,
                           timeout=300, env=cls.driver_env)
        cls.example = (ROOT / "examples/session-01/chain.log").read_bytes()
        cls.first, cls.second = cls.example.splitlines(keepends=True)[:2]
        cls.key = cls.run_driver("sign", cls.write("empty-body", b"")).splitlines()[0]
        cls.body = (b"jashproof/1\nchain_entries: 0\nchain_tip: " + b"0" * 64 +
                    b"\npubkey: " + cls.key + b"\nsig_algo: ed25519\n")
        cls.valid_proof = cls.signed(cls.body)
        print("Isolated build:", cls.work, flush=True)

    @classmethod
    def tearDownClass(cls):
        if cls.temp:
            cls.temp.cleanup()

    @classmethod
    def write(cls, name, data):
        path = cls.work / name
        path.write_bytes(data)
        return path

    @classmethod
    def run_driver(cls, *args):
        result = subprocess.run([str(cls.driver), *map(str, args)],
                                capture_output=True, timeout=15, env=cls.driver_env)
        if result.returncode:
            raise AssertionError(f"driver exit {result.returncode}: " +
                                 result.stderr.decode(errors="replace"))
        if result.stderr:
            raise AssertionError(result.stderr.decode(errors="replace"))
        return result.stdout

    @classmethod
    def signed(cls, body):
        signature = cls.run_driver("sign", cls.write("body", body)).splitlines()[1]
        return body + b"sig: " + signature + b"\n"

    def chain(self, data, expected=1, mode="chain"):
        path = self.write("chain", data)
        rc, count = map(int, self.run_driver(mode, path).split())
        self.assertEqual(rc, expected)
        if expected:
            self.assertEqual(count, 0, "partial prefix must not look fully verified")
        return count

    def proof(self, data, expected=1):
        path = self.write("proof", data)
        for mode in ("proof", "extract", "pinned"):
            with self.subTest(api=mode):
                args = (self.key.decode(),) if mode == "pinned" else ()
                self.assertEqual(int(self.run_driver(mode, path, *args).splitlines()[0]),
                                 expected)

    def record(self, cmd=b"true", ts=1000000001, rc=0):
        return self.run_driver("record", cmd.decode(), ts, rc)

    def test_legacy_examples(self):
        self.assertEqual(self.chain(self.example, 0), 6)
        for name in ("session.jashproof", "session.jashproof.dsse"):
            path = ROOT / "examples/session-01" / name
            self.assertEqual(self.run_driver("proof", path), b"0\n")

    def test_empty_and_missing_chain(self):
        self.assertEqual(self.chain(b"", 0), 0)
        self.assertEqual(self.run_driver("chain", self.work / "missing"), b"-1 0\n")

    def test_directory_is_read_error(self):
        self.assertEqual(self.run_driver("chain", self.work), b"-1 0\n")

    def test_read_error_after_valid_prefix(self):
        self.chain(self.first, -1, "readerror")

    def test_nul_suffix_after_valid_record(self):
        self.chain(self.first[:-1] + b"\0hidden\n")

    def test_chunk_boundary_is_not_record_boundary(self):
        first = self.first[:-1]
        first = first.replace(b" rc=", b" " * (1023 - len(first)) + b" rc=")
        self.assertEqual(len(first), 1023)
        self.chain(first + self.second)

    def test_long_producer_commands(self):
        for length in (1500, 4083):
            with self.subTest(length=length):
                self.assertEqual(self.chain(self.record(b"x" * length), 0), 1)

    def test_current_producer(self):
        binary = Path(os.environ.get("JASH_BIN", ROOT / "build/jash"))
        if not binary.is_file():
            self.skipTest("no existing jash binary; synthetic producer-format coverage remains")
        home = self.work / "producer-home"
        subprocess.run(["bash", str(ROOT / "tests/setup_sigil_modules.sh"), str(home)],
                       check=True, capture_output=True)
        env = {**os.environ, "HOME": str(home), "JASH_SIGIL": "2",
               "JASH_SESSION": "libjef-input-bounds"}
        proof = self.work / "producer.jashproof"
        dsse = self.work / "producer.dsse"
        for command in ("sigil record " + "x" * 1500, "sigil proof emit " + str(proof),
                        "sigil proof dsse " + str(dsse)):
            result = subprocess.run([str(binary), "-c", command], env=env,
                                    capture_output=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stderr)
        chains = list((home / ".jash").glob("chain-*.log"))
        self.assertTrue(chains)
        self.assertTrue(any(b" cmd=" + b"x" * 1500 + b"\n" in p.read_bytes()
                            for p in chains), "producer must emit the complete long command")
        for path in chains:
            self.chain(path.read_bytes(), 0)
        for path in (proof, dsse):
            self.assertEqual(self.run_driver("proof", path), b"0\n")

    @unittest.skipUnless(os.environ.get("JEF_ADJACENT") == "1", "opt-in receipt suites")
    def test_existing_receipt_suites(self):
        binary = os.environ.get("JASH_AGENTD_BIN", str(ROOT / "build/jash-agentd"))
        for name in ("test_libjef_job_receipt.sh", "test_libjef_job_spec.sh"):
            with self.subTest(suite=name), tempfile.TemporaryDirectory(
                    prefix="receipts-", dir=self.work) as tmp:
                source = (ROOT / "tests" / name).read_text()
                body = source.split("<<'PY'\n", 1)[1].rsplit("\nPY", 1)[0]
                if name == "test_libjef_job_spec.sh":
                    args = [binary, str(self.work / "jef_verify"), tmp, str(ROOT)]
                else:
                    args = [binary, str(self.work / "jef_verify"), sys.executable, tmp]
                subprocess.run([sys.executable, "-c", body, *args], check=True,
                               timeout=120, env=self.driver_env)

    def test_metadata_is_display_only(self):
        data = self.record(b"echo rc=9 cmd=x")
        for metadata in (b"cwd=/tmp repo=example dur=12 sess=demo ",
                         b"cwd=" + b"x" * 5000 + b" repo= "):
            with self.subTest(length=len(metadata)):
                self.chain(data.replace(b"cmd=", metadata + b"cmd=", 1), 0)

    def test_no_header_lookup_inside_command(self):
        data = self.record(b"echo rc=0")
        self.chain(data.replace(b" rc=0 cmd=", b" cmd=", 1))

    def test_chain_framing_and_limits(self):
        for data in (self.first[:-1], self.first + b"bad", self.first + b"\0\n",
                     self.first + b"\n", b"x" * 16385 + b"\n",
                     self.record(b"x" * 4084),
                     self.first.replace(b" cmd=", b" missing=", 1)):
            with self.subTest(length=len(data)):
                self.chain(data)
        data = self.record(b"x" * 4083)
        at_limit = data.replace(b" cmd=", b" cwd=" + b"x" * (16384 - len(data) - 5) +
                                b" cmd=", 1)
        self.assertEqual(len(at_limit), 16384)
        self.chain(at_limit, 0)
        self.chain(at_limit.replace(b" cwd=", b" cwd=x", 1))

    def test_numeric_and_hash_tokens_are_exact(self):
        ts, hash_value = self.first.split()[1:3]
        for old, new in ((ts, ts + b"junk"), (b"rc=0", b"rc=0junk"),
                         (hash_value, hash_value + b"junk")):
            with self.subTest(token=old):
                self.chain(self.first.replace(old, new, 1))

    def test_numeric_grammar_and_overflow(self):
        data = self.record()
        for token in (b"+0", b"-0", b"00", b"18446744073709551616"):
            with self.subTest(seq=token):
                self.chain(token + data[1:])
        for token in (b"1.1", b"1.000000001x", b"1.1000000000", b"+1.000000001",
                      b"01.000000001", b"18446744073.709551616",
                      b"18446744073709551616.000000000"):
            with self.subTest(ts=token):
                self.chain(data.replace(b"1.000000001", token))
        for token in (b"+0", b"-0", b"00", b"2147483648", b"-2147483649",
                      b"4294967296", b"0x0", b"", b"0 rc=0"):
            with self.subTest(rc=token):
                self.chain(data.replace(b"rc=0", b"rc=" + token))
        for rc in (-2147483648, -1, 2147483647):
            with self.subTest(valid_rc=rc):
                self.chain(self.record(rc=rc, ts=18446744073709551615), 0)

    def test_text_valid_lf_policy_and_signed_extensions(self):
        self.proof(self.valid_proof, 0)
        self.proof(self.valid_proof[:-1], 0)
        self.proof(self.signed(self.body + b"extension: arbitrary signed text\n"), 0)

    def test_text_signature_garbage_and_unsigned_tail(self):
        for tail in (b"garbage", b"\nmetadata: unsigned\n", b"\nsig: " + b"0" * 128,
                     b"\n\n", b"\0", b"\r\n"):
            with self.subTest(tail=tail[:30]):
                self.proof(self.valid_proof[:-1] + tail)

    def test_unsigned_key_algorithm_and_metadata(self):
        self.proof(self.signed(b"\n") + self.body[11:])

    def test_text_required_fields(self):
        for line in self.body.splitlines(keepends=True):
            with self.subTest(missing=line[:24]):
                self.proof(self.signed(self.body.replace(line, b"", 1)))
            with self.subTest(duplicate=line[:24]):
                self.proof(self.signed(self.body + line))

    def test_text_exact_field_values(self):
        for old, new in ((b"ed25519", b"ed25519junk"), (self.key, self.key + b"0"),
                         (b"chain_entries: 0", b"chain_entries: 0junk"),
                         (b"chain_entries: 0", b"chain_entries: -1"),
                         (b"chain_entries: 0", b"chain_entries: 18446744073709551616"),
                         (b"chain_tip: " + b"0" * 64, b"chain_tip: " + b"g" * 64),
                         (b"chain_tip: " + b"0" * 64, b"chain_tip: " + b"0" * 65),
                         (b"pubkey: ", b"extension: pubkey: ")):
            with self.subTest(value=new[:40]):
                self.proof(self.signed(self.body.replace(old, new)))

    def test_text_malformed_corpus(self):
        for data in (b"", b"sig: " + b"0" * 128, b" " + self.valid_proof,
                     self.valid_proof[:-2], self.valid_proof.replace(b"sig: ", b"sig:"),
                     self.valid_proof.replace(b"sig: ", b"sig: g"),
                     self.valid_proof + b"x" * 8192):
            with self.subTest(length=len(data)):
                self.proof(data)

    def test_text_buffer_edge_prefixes(self):
        prefix = b"sig_algo: ed25519\nsig: " + b"0" * 128 + b"\n"
        for offset in range(65):
            with self.subTest(offset=offset):
                tail = b"pubkey: " + b"0" * offset
                self.proof(prefix + b"x" * (8191 - len(prefix) - len(tail)) + tail)
                # Also reach the length-aware decoder past a valid header.
                start = b"jashproof/1\nsig_algo: ed25519\nextension: "
                self.proof(start + b"x" * (8190 - len(start) - len(tail)) + b"\n" + tail)

    def test_pinned_key_sanity(self):
        path = self.write("proof", self.valid_proof)
        for key in ("", "0" * 63, "0" * 65, "g" * 64):
            with self.subTest(key=key):
                self.assertEqual(self.run_driver("pinned", path, key), b"-2\n")
        self.assertEqual(self.run_driver("pinned", path, self.key.decode().upper()), b"0\n")
        self.assertEqual(self.run_driver("pinned", path, "0" * 64), b"1\n")

    def test_pinned_verification_uses_one_opened_proof(self):
        path = self.write("race-proof", self.valid_proof)
        replacement = self.write("race-replacement",
                                 self.valid_proof.replace(self.key, b"0" * 64, 1))
        self.assertEqual(self.run_driver("pinnedrace", path, "0" * 64, replacement),
                         b"1\n")


if __name__ == "__main__":
    unittest.main(verbosity=2)
