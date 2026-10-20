// SPDX-License-Identifier: LicenseRef-SPL-1.0
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../libjef/jef_verify.h"

int main(int argc, char** argv) {
    assert(argc == 3);
    FILE* file = fopen(argv[1], "rb"); assert(file);
    char bytes[8192]; size_t n = fread(bytes, 1, sizeof(bytes) - 1, file);
    assert(!ferror(file) && feof(file) && n); fclose(file); bytes[n] = 0;
    assert(jef_verify_proof_bytes_pinned(bytes, (unsigned)n, argv[2]) == 0);
    assert(jef_verify_proof_bytes_pinned(bytes, (unsigned)n, "0000000000000000000000000000000000000000000000000000000000000000") != 0);
    assert(jef_verify_proof_bytes_pinned(bytes, (unsigned)n, "bad") == -2);
    assert(jef_verify_proof_bytes_pinned(bytes, sizeof(bytes), argv[2]) != 0);
    char* payload = strstr(bytes, "\"payload\":\""); assert(payload);
    payload += strlen("\"payload\":\""); assert(strlen(payload) > 20);
    payload[10] = payload[10] == 'A' ? 'B' : 'A';
    assert(jef_verify_proof_bytes_pinned(bytes, (unsigned)n, argv[2]) != 0);
    return 0;
}
