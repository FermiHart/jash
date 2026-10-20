// SPDX-License-Identifier: LicenseRef-SPL-1.0
/* Test-only real C validator for either explicitly selected byte adapter. */
#include "core/data_wire.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(void) {
    size_t limit = 64u * 1024u * 1024u;
    unsigned char* wire = malloc(limit + 1u); void* storage = malloc(1024u * 1024u);
    if (!wire || !storage) { free(wire); free(storage); return 125; }
    size_t count = fread(wire, 1, limit + 1u, stdin);
    int result = 1;
    if (!ferror(stdin) && count <= limit && count >= 4u) {
        jdv_wire_limits_t bounds; jdv_wire_limits_init(&bounds);
        bounds.max_wire_bytes = limit; bounds.max_frame_bytes = 128u * 1024u; bounds.max_items = 65539u;
        bounds.value_limits.max_encoded_bytes = 128u * 1024u;
        bounds.value_limits.max_aggregate_bytes = 128u * 1024u;
        jdv_arena_t arena; jdv_arena_init(&arena, storage, 1024u * 1024u);
        jdv_wire_info_t info; int rc = -1;
        if (!memcmp(wire, "JTV1", 4)) rc = jdv_wire_validate(wire, count, &arena, &bounds, &info);
        else if (!memcmp(wire, "JTV2", 4)) rc = jdv_wire_validate_v2(wire, count, &arena, &bounds, &info);
        if (!rc) {
            printf("{\"state\":%d,\"items\":%llu,\"bytes\":%llu,\"logical_verified\":%s}\n",
                (int)info.state, (unsigned long long)info.items_seen, (unsigned long long)info.bytes_seen,
                info.logical_verified ? "true" : "false");
            result = 0;
        } else fprintf(stderr, "invalid typed wire: errno=%d\n", errno);
    }
    free(wire); free(storage); return result;
}
