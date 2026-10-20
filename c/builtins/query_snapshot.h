// SPDX-License-Identifier: LicenseRef-SPL-1.0
#ifndef JASH_BUILTINS_QUERY_SNAPSHOT_H
#define JASH_BUILTINS_QUERY_SNAPSHOT_H
#include <core/state.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>

typedef struct {
    struct stat metadata;
    unsigned char* bytes;
    size_t used;
    uint8_t digest[32];
    int error_number;
    const char* error;
} jash_query_file_page_t;

/* Caller frees page.bytes on every outcome. Uses one descriptor and checks its
 * identity and timestamps against the named file before publishing bytes. */
int jash_query_file_page(jash_state_t*, const char* path, uint64_t offset,
                          size_t limit, jash_query_file_page_t*);
#endif
