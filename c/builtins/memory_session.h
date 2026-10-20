// SPDX-License-Identifier: LicenseRef-SPL-1.0
#ifndef JASH_BUILTINS_MEMORY_SESSION_H
#define JASH_BUILTINS_MEMORY_SESSION_H
#include "memory_store.h"
typedef struct {
    uint64_t timestamp, duration;
    int64_t status;
    char* command;
    int maybe_truncated;
} memory_session_record_t;
/* Caller supplies one complete NUL-free line, without its final newline.
 * Only the first three separators delimit fields; command pipes are literal.
 * This parses the retained unescaped record, not the truth of its command. */
static inline int memory_session_parse(char* line, size_t length, memory_session_record_t* record) {
    *record = (memory_session_record_t){.maybe_truncated = length == 2040};
    char* fields[4] = {line,NULL,NULL,NULL};
    for (size_t i = 1; i < 4; i++) {
        char* delimiter = strchr(fields[i - 1], '|');
        if (!delimiter) { errno = EINVAL; return -1; }
        *delimiter = 0; fields[i] = delimiter + 1;
    }
    if (memory_unsigned(fields[0], UINT64_MAX, &record->timestamp) ||
        memory_integer(fields[1], &record->status) ||
        memory_unsigned(fields[2], UINT64_MAX, &record->duration) || !fields[3][0]) {
        errno = EINVAL; return -1;
    }
    record->command = fields[3]; return 0;
}
#endif
