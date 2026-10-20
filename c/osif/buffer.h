// SPDX-License-Identifier: LicenseRef-SPL-1.0
#ifndef JASH_OSIF_BUFFER_H
#define JASH_OSIF_BUFFER_H
#include <stddef.h>
#include <stdint.h>
/* Returned descriptors are owned/CLOEXEC. Maps borrow the descriptor. Validation
 * never copies or silently falls back to a weaker immutable guarantee. */
int jash_buffer_compiled(void);
int jash_buffer_probe(void);
int jash_buffer_create(const void*, size_t, uint64_t limit);
int jash_buffer_validate(int fd, uint64_t size, uint64_t limit);
int jash_buffer_map(int fd, uint64_t size, uint64_t limit, const void**);
void jash_buffer_unmap(const void*, uint64_t size);
/* Opens the same sealed object read-only with an independent seek position. */
int jash_buffer_reader_fd(int fd, uint64_t size, uint64_t limit);
int jash_buffer_copy_range_fd(int source, uint64_t offset, uint64_t length,
                              uint64_t limit);
int jash_buffer_copy_fd(int source, uint64_t limit);
typedef int (*jash_buffer_observer)(void *user,const void *bytes,size_t length,uint64_t offset);
typedef struct { uint64_t read_bytes,copied_bytes; } jash_buffer_copy_stats;
/* Read a regular source once, observing the same chunks used to stage only the
 * selected range. Snapshot is returned sealed only after metadata is unchanged.
 * The caller verifies its content commitment before publishing the descriptor.
 * Counters describe actual IO, including a partial attempt on failure. */
int jash_buffer_copy_observed_range_fd(int source,uint64_t source_size,
    uint64_t offset,uint64_t length,uint64_t limit,jash_buffer_observer observer,
    void *user,jash_buffer_copy_stats *stats);
#endif
