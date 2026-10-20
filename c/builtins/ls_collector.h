// SPDX-License-Identifier: LicenseRef-SPL-1.0
#ifndef JASH_LS_COLLECTOR_H
#define JASH_LS_COLLECTOR_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    const unsigned char* data;
    size_t len;
} jash_ls_bytes_t;

typedef enum {
    JASH_LS_TYPE_REGULAR,
    JASH_LS_TYPE_DIRECTORY,
    JASH_LS_TYPE_SYMLINK,
    JASH_LS_TYPE_FIFO,
    JASH_LS_TYPE_SOCKET,
    JASH_LS_TYPE_BLOCK,
    JASH_LS_TYPE_CHARACTER,
    JASH_LS_TYPE_UNKNOWN
} jash_ls_type_t;

typedef struct {
    int64_t seconds;
    uint32_t nanoseconds;
    int available;
} jash_ls_timestamp_t;

enum {
    JASH_LS_META_SIZE   = 1u << 0,
    JASH_LS_META_MODE   = 1u << 1,
    JASH_LS_META_UID    = 1u << 2,
    JASH_LS_META_GID    = 1u << 3,
    JASH_LS_META_INODE  = 1u << 4,
    JASH_LS_META_DEVICE = 1u << 5,
    JASH_LS_META_NLINK  = 1u << 6
};

typedef struct {
    uint32_t available;
    uint64_t size;
    uint64_t mode;
    uint64_t uid;
    uint64_t gid;
    uint64_t inode;
    uint64_t device;
    uint64_t nlink;
    jash_ls_timestamp_t modified;
    jash_ls_timestamp_t changed;
    jash_ls_timestamp_t accessed;
    jash_ls_timestamp_t created;
} jash_ls_metadata_t;

typedef enum {
    JASH_LS_ITEM_ERROR_NONE,
    JASH_LS_ITEM_ERROR_METADATA,
    JASH_LS_ITEM_ERROR_READLINK,
    JASH_LS_ITEM_ERROR_UNSTABLE
} jash_ls_item_error_op_t;

typedef struct {
    jash_ls_item_error_op_t operation;
    int error_number;
} jash_ls_item_error_t;

typedef enum {
    JASH_LS_CONSISTENCY_UNAVAILABLE,
    JASH_LS_CONSISTENCY_DESCRIPTOR_BOUND,
    JASH_LS_CONSISTENCY_PATH_FALLBACK,
    JASH_LS_CONSISTENCY_UNSTABLE
} jash_ls_consistency_t;

typedef struct {
    jash_ls_bytes_t name;
    jash_ls_bytes_t path;
    jash_ls_type_t type;
    jash_ls_metadata_t metadata;
    jash_ls_bytes_t symlink_target;
    int symlink_target_available;
    int symlink_target_truncated;
    jash_ls_consistency_t consistency;
    jash_ls_item_error_t error;
} jash_ls_item_t;

typedef enum {
    JASH_LS_COLLECT_DIRECTORY,
    JASH_LS_COLLECT_OPERAND
} jash_ls_collect_mode_t;

typedef enum {
    JASH_LS_SYMLINK_NOFOLLOW,
    JASH_LS_SYMLINK_FOLLOW
} jash_ls_symlink_policy_t;

typedef struct {
    jash_ls_bytes_t root;
    jash_ls_collect_mode_t mode;
    jash_ls_symlink_policy_t symlink_policy;
    /* Directory operands may follow their final symlink; entries retain the
     * independent symlink_policy. Defaults to no-follow for new consumers. */
    int follow_root_symlink;
    int include_hidden;
    size_t max_items;
    size_t max_logical_bytes;
    size_t max_memory_bytes;
    size_t max_symlink_bytes;
} jash_ls_collect_options_t;

typedef enum {
    JASH_LS_TERMINAL_COMPLETE,
    JASH_LS_TERMINAL_TRUNCATED,
    JASH_LS_TERMINAL_CANCELLED,
    JASH_LS_TERMINAL_PRODUCER_ERROR,
    JASH_LS_TERMINAL_SINK_ERROR,
    JASH_LS_TERMINAL_PENDING
} jash_ls_terminal_state_t;

typedef struct {
    jash_ls_terminal_state_t state;
    size_t items_seen;
    size_t items_emitted;
    size_t logical_bytes_emitted;
    size_t errors_seen;
    size_t symlink_targets_truncated;
    size_t retained_bytes_peak;
    size_t collector_bytes_peak;
    int error_number;
} jash_ls_collect_summary_t;

typedef enum {
    JASH_LS_CALLBACK_CONTINUE,
    JASH_LS_CALLBACK_CANCEL,
    JASH_LS_CALLBACK_ERROR
} jash_ls_callback_result_t;

/* All inputs are borrowed and calls are synchronous. The collector snapshots
 * the complete options value, root bytes, callback, and internal summary before
 * traversal. Item slices remain valid only until the callback returns.
 *
 * max_items and max_logical_bytes bound emitted rows and their name + path +
 * retained target bytes. max_memory_bytes separately bounds all allocations
 * made by the collector; retained_bytes_peak and collector_bytes_peak report
 * retained and total allocation high-water marks. All three limits are
 * mandatory and nonzero. max_symlink_bytes bounds target bytes; zero disables
 * target reads.
 *
 * Directory rows are sorted by unsigned name bytes. A limited result is always
 * a prefix of the result obtained with larger limits. Hidden filtering applies
 * only to directory contents, so explicit `.` and `..` operands are observable.
 * Directory enumeration itself never emits `.` or `..`.
 *
 * symlink_policy controls metadata lookup. On Linux, O_PATH descriptors bind
 * metadata and readlink to one object where supported. Other systems and Linux
 * filesystems lacking that facility use a checked path-based fallback, reported
 * by item.consistency; unavailable metadata is reported separately, and a
 * detected replacement is an UNSTABLE per-item error.
 *
 * CONTINUE accepts a row. CANCEL accepts that row and ends with CANCELLED.
 * ERROR rejects it and ends with SINK_ERROR, using errno or EIO. Complete,
 * truncated, and cancelled collections return 0. Producer and sink errors
 * return -1 with errno set. The final summary is copied out after cleanup. */
typedef jash_ls_callback_result_t (*jash_ls_item_callback_t)(
    void* context, const jash_ls_item_t* item);

int jash_ls_collect(const jash_ls_collect_options_t* options,
                    jash_ls_item_callback_t callback,
                    void* context,
                    jash_ls_collect_summary_t* summary);

/* Resumable read-only traversal. The cursor is caller-owned, non-copyable and
 * must stay at its original address until close. Only names are pre-sorted and
 * retained within max_memory_bytes; metadata is fetched on each next(). Every
 * returned item and its slices are borrowed until next(), cancel(), or close().
 * END is idempotent; producer errors are distinct from a truncated terminal.
 * A successful open must always be paired with close (including cancellation). */
typedef struct jash_ls_cursor {
    void* _state;
    const struct jash_ls_cursor* _self;
} jash_ls_cursor_t;

typedef enum {
    JASH_LS_NEXT_ITEM = 1,
    JASH_LS_NEXT_END = 0,
    JASH_LS_NEXT_ERROR = -1
} jash_ls_next_result_t;

int jash_ls_cursor_open(jash_ls_cursor_t* cursor,
                        const jash_ls_collect_options_t* options);
jash_ls_next_result_t jash_ls_cursor_next(jash_ls_cursor_t* cursor,
                                          const jash_ls_item_t** item);
int jash_ls_cursor_cancel(jash_ls_cursor_t* cursor);
int jash_ls_cursor_summary(const jash_ls_cursor_t* cursor,
                           jash_ls_collect_summary_t* summary);
int jash_ls_cursor_close(jash_ls_cursor_t* cursor);

#endif
