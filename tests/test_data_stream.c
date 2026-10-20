// SPDX-License-Identifier: LicenseRef-SPL-1.0
#include <core/data_stream.h>
#include <module/blake3.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL line %d: %s (errno=%d)\n", \
                __LINE__, #condition, errno); \
        return 1; \
    } \
} while (0)

typedef struct {
    jdv_stream_pull_result_t result;
    const jdv_value_t* item;
    const jdv_stream_end_t* terminal;
    uint64_t logical;
    uint64_t physical;
    uint64_t materialized;
} step_t;

typedef struct {
    const step_t* steps;
    size_t step_count;
    size_t next_step;
    int pulls;
    int cancels;
    int releases;
    int reenter;
    int reenter_pull_errno;
    int reenter_cancel_errno;
    int reenter_release_errno;
    int cancel_status;
    jdv_stream_cursor_t* cursor;
    jdv_value_t borrowed;
} producer_t;

static jdv_stream_pull_result_t scripted_pull(void* context,
                                              jdv_stream_record_t* record) {
    producer_t* producer = context;
    producer->pulls++;
    if (producer->reenter) {
        jdv_stream_record_t nested;
        if (jdv_stream_pull(producer->cursor, &nested) == JDV_PULL_ERROR)
            producer->reenter_pull_errno = errno;
        if (jdv_stream_cancel(producer->cursor, JDV_LITERAL("nested"),
                              NULL) < 0)
            producer->reenter_cancel_errno = errno;
        if (jdv_stream_release(producer->cursor) < 0)
            producer->reenter_release_errno = errno;
        producer->reenter = 0;
    }
    if (producer->next_step >= producer->step_count) {
        record->terminal = NULL;
        return JDV_PULL_END;
    }
    const step_t* step = &producer->steps[producer->next_step++];
    record->item = step->item;
    record->terminal = step->terminal;
    record->logical_bytes = step->logical;
    record->physical_bytes = step->physical;
    record->materialized_bytes = step->materialized;
    return step->result;
}

static int scripted_cancel(void* context, jdv_slice_t reason) {
    producer_t* producer = context;
    producer->cancels++;
    producer->borrowed = jdv_value_u64(UINT64_C(0xdead));
    if (producer->reenter && producer->cursor) {
        const jdv_stream_end_t* terminal;
        if (jdv_stream_cancel(producer->cursor, reason, &terminal) < 0)
            producer->reenter_cancel_errno = errno;
    }
    return producer->cancel_status;
}

static void scripted_release(void* context) {
    producer_t* producer = context;
    producer->releases++;
    producer->borrowed = jdv_value_u64(UINT64_C(0xbeef));
    if (producer->reenter && producer->cursor) {
        if (jdv_stream_release(producer->cursor) < 0)
            producer->reenter_release_errno = errno;
    }
}

static const jdv_stream_producer_vtable_t VTABLE = {
    JDV_STREAM_VTABLE_VERSION,
    sizeof(jdv_stream_producer_vtable_t),
    scripted_pull,
    scripted_cancel,
    scripted_release,
};

static int complete_end(jdv_stream_end_t* end, uint64_t items,
                        uint64_t logical, const jdv_schema_t* schema) {
    if (jdv_stream_end_init(end) < 0) return -1;
    end->state = JDV_STREAM_COMPLETE;
    end->items_seen = items;
    end->bytes_seen = logical;
    end->completeness = JDV_COMPLETENESS_COMPLETE;
    end->final_schema = schema;
    end->final_hash_present = 1;
    end->final_hash_algorithm = JDV_HASH_BLAKE3;
    blake3_hash("stream", 6, end->final_hash);
    return 0;
}

static int failed_end(jdv_stream_end_t* end, uint64_t items,
                       uint64_t logical, int producer_status) {
    if (jdv_stream_end_init(end) < 0) return -1;
    end->state = JDV_STREAM_FAILED;
    end->items_seen = items;
    end->bytes_seen = logical;
    end->errors_seen = 1;
    end->producer_status = producer_status;
    end->transform_status = 23;
    end->transport_status = 29;
    end->completeness = items || logical
        ? JDV_COMPLETENESS_PARTIAL : JDV_COMPLETENESS_UNKNOWN;
    return 0;
}

static int incomplete_end(jdv_stream_end_t* end, int producer_status) {
    if (jdv_stream_end_init(end) < 0) return -1;
    end->state = JDV_STREAM_INCOMPLETE;
    end->producer_status = producer_status;
    end->completeness = JDV_COMPLETENESS_UNKNOWN;
    return 0;
}

static int open_stream(producer_t* producer, const jdv_schema_t* declared,
                       const jdv_schema_t* guaranteed,
                       const jdv_stream_limits_t* limits,
                       jdv_stream_descriptor_t* descriptor,
                       jdv_stream_cursor_t* cursor) {
    jdv_stream_config_t config;
    jdv_stream_config_init(&config);
    config.boundedness = JDV_STREAM_FINITE;
    config.declared_schema = declared;
    config.guaranteed_schema = guaranteed;
    if (limits) config.stream_limits = *limits;
    if (jdv_stream_descriptor_init(descriptor, &config, &VTABLE,
                                   producer) < 0)
        return -1;
    if (jdv_stream_open(descriptor, cursor) < 0) return -1;
    producer->cursor = cursor;
    return 0;
}

static int test_normal_completion(void) {
    jdv_schema_t schema;
    CHECK(jdv_schema_scalar(JDV_U64, 0, &schema) == 0);
    jdv_value_t one = jdv_value_u64(1), two = jdv_value_u64(2);
    jdv_stream_end_t end;
    CHECK(complete_end(&end, 2, 7, &schema) == 0);
    end.producer_status = 11;
    end.transform_status = 12;
    end.transport_status = 13;
    const step_t steps[] = {
        {JDV_PULL_ITEM, &one, NULL, 3, 4, 1},
        {JDV_PULL_ITEM, &two, NULL, 4, 5, 3},
        {JDV_PULL_END, NULL, &end, 0, 2, 0},
    };
    producer_t producer = {.steps = steps, .step_count = 3};
    jdv_stream_descriptor_t descriptor;
    jdv_stream_cursor_t cursor;
    CHECK(open_stream(&producer, &schema, &schema, NULL,
                      &descriptor, &cursor) == 0);

    const jdv_schema_set_t* schemas;
    CHECK(jdv_stream_schemas(&cursor, &schemas) == 0);
    CHECK(schemas->declared_schema == &schema &&
          schemas->validation_scope == JDV_VALIDATION_NONE);
    const jdv_stream_end_t* queried;
    CHECK(jdv_stream_terminal(&cursor, &queried) < 0 && errno == ENODATA);

    jdv_stream_record_t record;
    CHECK(jdv_stream_pull(&cursor, &record) == JDV_PULL_ITEM);
    CHECK(record.item == &one && jdv_stream_item_valid(&cursor, &record));
    const jdv_stream_schema_lifecycle_t* lifecycle;
    CHECK(jdv_stream_schema_lifecycle(&cursor, &lifecycle) == 0);
    CHECK(lifecycle->observed_items == 1 &&
          lifecycle->observed_prefix_valid == 1 &&
          lifecycle->set.validation_scope == JDV_VALIDATION_PREFIX &&
          lifecycle->set.observed_schema == &schema && !lifecycle->terminal);
    CHECK(jdv_stream_pull(&cursor, &record) == JDV_PULL_ITEM);
    CHECK(record.item == &two && jdv_stream_item_valid(&cursor, &record));
    CHECK(jdv_stream_pull(&cursor, &record) == JDV_PULL_END);
    CHECK(record.terminal && record.terminal->state == JDV_STREAM_COMPLETE);
    CHECK(record.terminal->producer_status == 11);
    CHECK(record.terminal->transform_status == 12);
    CHECK(record.terminal->transport_status == 13);
    CHECK(!jdv_stream_item_valid(&cursor, &record));

    const jdv_stream_end_t* first;
    const jdv_stream_end_t* second;
    CHECK(jdv_stream_terminal(&cursor, &first) == 0);
    CHECK(jdv_stream_terminal(&cursor, &second) == 0 && first == second);
    CHECK(jdv_stream_schemas(&cursor, &schemas) == 0);
    CHECK(schemas->validation_scope == JDV_VALIDATION_COMPLETE &&
          schemas->final_schema == &schema);
    CHECK(jdv_stream_schema_lifecycle(&cursor, &lifecycle) == 0);
    CHECK(lifecycle->terminal &&
          lifecycle->terminal_state == JDV_STREAM_COMPLETE);
    jdv_stream_accounting_t accounting;
    CHECK(jdv_stream_accounting(&cursor, &accounting) == 0);
    CHECK(accounting.items == 2 && accounting.logical_bytes == 7);
    CHECK(accounting.physical_bytes == 11 &&
          accounting.materialized_bytes == 4);
    CHECK(jdv_stream_pull(&cursor, &record) == JDV_PULL_ERROR &&
          errno == EALREADY && record.terminal == NULL);
    CHECK(producer.pulls == 3);
    CHECK(jdv_stream_release(&cursor) == 0 && producer.releases == 1);
    CHECK(jdv_stream_terminal(&cursor, &queried) == 0 && queried == first);
    return 0;
}

static int test_empty_and_duplicate_end(void) {
    jdv_schema_t schema;
    CHECK(jdv_schema_scalar(JDV_U64, 0, &schema) == 0);
    jdv_stream_end_t first_end, duplicate_end;
    CHECK(complete_end(&first_end, 0, 0, &schema) == 0);
    CHECK(complete_end(&duplicate_end, 0, 0, &schema) == 0);
    const step_t steps[] = {
        {JDV_PULL_END, NULL, &first_end, 0, 0, 0},
        {JDV_PULL_END, NULL, &duplicate_end, 0, 0, 0},
    };
    producer_t producer = {.steps = steps, .step_count = 2};
    jdv_stream_descriptor_t descriptor;
    jdv_stream_cursor_t cursor;
    CHECK(open_stream(&producer, &schema, &schema, NULL,
                      &descriptor, &cursor) == 0);
    jdv_stream_record_t record;
    CHECK(jdv_stream_pull(&cursor, &record) == JDV_PULL_END);
    CHECK(record.terminal->items_seen == 0);
    CHECK(jdv_stream_pull(&cursor, &record) == JDV_PULL_ERROR &&
          errno == EALREADY);
    CHECK(producer.pulls == 1 && producer.next_step == 1);
    CHECK(jdv_stream_release(&cursor) == 0);

    const step_t missing_step = {JDV_PULL_END, NULL, NULL, 0, 0, 0};
    producer_t missing = {.steps = &missing_step, .step_count = 1};
    jdv_stream_descriptor_t missing_descriptor;
    jdv_stream_cursor_t missing_cursor;
    CHECK(open_stream(&missing, &schema, &schema, NULL,
                      &missing_descriptor, &missing_cursor) == 0);
    CHECK(jdv_stream_pull(&missing_cursor, &record) == JDV_PULL_ERROR &&
          errno == EPROTO);
    CHECK(record.terminal->state == JDV_STREAM_FAILED &&
          record.terminal->producer_status == EPROTO);
    CHECK(jdv_stream_release(&missing_cursor) == 0);
    return 0;
}

static int test_cancellation(void) {
    jdv_schema_t schema;
    CHECK(jdv_schema_scalar(JDV_U64, 0, &schema) == 0);
    producer_t producer = {0};
    jdv_stream_descriptor_t descriptor;
    jdv_stream_cursor_t cursor;
    CHECK(open_stream(&producer, &schema, &schema, NULL,
                      &descriptor, &cursor) == 0);
    const jdv_stream_end_t* first;
    CHECK(jdv_stream_cancel(&cursor, JDV_LITERAL("stop"), &first) == 0);
    CHECK(first->state == JDV_STREAM_CANCELLED &&
          first->completeness == JDV_COMPLETENESS_UNKNOWN);
    CHECK(first->reason.len == 4 && !memcmp(first->reason.data, "stop", 4));
    const jdv_stream_end_t* second;
    CHECK(jdv_stream_cancel(&cursor, JDV_LITERAL("again"), &second) == 0);
    CHECK(first == second && producer.cancels == 1);
    jdv_stream_record_t record;
    CHECK(jdv_stream_pull(&cursor, &record) == JDV_PULL_ERROR &&
          errno == EALREADY);
    CHECK(jdv_stream_release(&cursor) == 0 && producer.releases == 1);
    CHECK(jdv_stream_cancel(&cursor, JDV_LITERAL("late"), NULL) < 0 &&
          errno == EALREADY);
    return 0;
}

static int test_producer_failure_and_pending(void) {
    jdv_schema_t schema;
    CHECK(jdv_schema_scalar(JDV_U64, 0, &schema) == 0);
    jdv_value_t value = jdv_value_u64(7);
    jdv_stream_end_t end;
    CHECK(failed_end(&end, 1, 2, 17) == 0);
    const step_t steps[] = {
        {JDV_PULL_PENDING, NULL, NULL, 0, 0, 0},
        {JDV_PULL_ITEM, &value, NULL, 2, 4, 5},
        {JDV_PULL_ERROR, NULL, &end, 0, 1, 0},
    };
    producer_t producer = {.steps = steps, .step_count = 3};
    jdv_stream_descriptor_t descriptor;
    jdv_stream_cursor_t cursor;
    CHECK(open_stream(&producer, &schema, &schema, NULL,
                      &descriptor, &cursor) == 0);
    jdv_stream_record_t record;
    CHECK(jdv_stream_pull(&cursor, &record) == JDV_PULL_PENDING);
    CHECK(record.item == NULL && record.terminal == NULL &&
          record.physical_bytes == 0);
    CHECK(jdv_stream_pull(&cursor, &record) == JDV_PULL_ITEM);
    errno = EBUSY;
    CHECK(jdv_stream_pull(&cursor, &record) == JDV_PULL_ERROR && errno == 0);
    CHECK(record.terminal->state == JDV_STREAM_FAILED);
    CHECK(record.terminal->producer_status == 17);
    CHECK(record.terminal->transform_status == 23);
    CHECK(record.terminal->transport_status == 29);
    const jdv_stream_schema_lifecycle_t* lifecycle;
    CHECK(jdv_stream_schema_lifecycle(&cursor, &lifecycle) == 0);
    CHECK(lifecycle->terminal &&
          lifecycle->terminal_state == JDV_STREAM_FAILED &&
          lifecycle->observed_items == 1);
    jdv_stream_accounting_t accounting;
    CHECK(jdv_stream_accounting(&cursor, &accounting) == 0);
    CHECK(accounting.items == 1 && accounting.logical_bytes == 2 &&
          accounting.physical_bytes == 5 &&
          accounting.materialized_bytes == 5);
    CHECK(jdv_stream_release(&cursor) == 0);

    jdv_stream_end_t incomplete;
    CHECK(incomplete_end(&incomplete, 31) == 0);
    const step_t incomplete_step = {
        JDV_PULL_ERROR, NULL, &incomplete, 0, 0, 0
    };
    producer_t incomplete_producer = {
        .steps = &incomplete_step, .step_count = 1
    };
    CHECK(open_stream(&incomplete_producer, &schema, &schema, NULL,
                      &descriptor, &cursor) == 0);
    errno = ERANGE;
    CHECK(jdv_stream_pull(&cursor, &record) == JDV_PULL_ERROR && errno == 0);
    CHECK(record.terminal->state == JDV_STREAM_INCOMPLETE &&
          record.terminal->producer_status == 31);
    CHECK(jdv_stream_schema_lifecycle(&cursor, &lifecycle) == 0);
    CHECK(lifecycle->terminal &&
          lifecycle->terminal_state == JDV_STREAM_INCOMPLETE);
    CHECK(jdv_stream_release(&cursor) == 0);

    jdv_stream_end_t cancelled;
    CHECK(jdv_stream_end_init(&cancelled) == 0);
    cancelled.state = JDV_STREAM_CANCELLED;
    cancelled.completeness = JDV_COMPLETENESS_PARTIAL;
    cancelled.reason = JDV_LITERAL("upstream cancelled");
    const step_t cancelled_step = {JDV_PULL_ERROR, NULL, &cancelled, 0, 0, 0};
    producer_t cancelled_producer = {
        .steps = &cancelled_step, .step_count = 1
    };
    CHECK(open_stream(&cancelled_producer, &schema, &schema, NULL,
                      &descriptor, &cursor) == 0);
    CHECK(jdv_stream_pull(&cursor, &record) == JDV_PULL_ERROR);
    CHECK(record.terminal && record.terminal->state == JDV_STREAM_CANCELLED);
    CHECK(jdv_stream_release(&cursor) == 0);
    CHECK(cancelled_producer.cancels == 0 && cancelled_producer.releases == 1);
    return 0;
}

static int test_limits_and_overflow(void) {
    jdv_schema_t schema;
    CHECK(jdv_schema_scalar(JDV_U64, 0, &schema) == 0);
    jdv_value_t value = jdv_value_u64(1);
    step_t too_large[] = {{JDV_PULL_ITEM, &value, NULL, 5, 1, 1}};
    producer_t limited_producer = {.steps = too_large, .step_count = 1};
    jdv_stream_limits_t limits;
    jdv_stream_limits_init(&limits);
    limits.max_logical_bytes = 4;
    jdv_stream_descriptor_t descriptor;
    jdv_stream_cursor_t cursor;
    CHECK(open_stream(&limited_producer, &schema, &schema, &limits,
                      &descriptor, &cursor) == 0);
    jdv_stream_record_t record;
    CHECK(jdv_stream_pull(&cursor, &record) == JDV_PULL_ERROR &&
          errno == E2BIG);
    CHECK(record.terminal->transform_status == E2BIG &&
          record.terminal->state == JDV_STREAM_FAILED);
    CHECK(jdv_stream_release(&cursor) == 0);

    const step_t overflow_steps[] = {
        {JDV_PULL_ITEM, &value, NULL, UINT64_MAX, 0, 0},
        {JDV_PULL_ITEM, &value, NULL, 1, 0, 0},
    };
    producer_t overflow_producer = {
        .steps = overflow_steps, .step_count = 2
    };
    CHECK(open_stream(&overflow_producer, &schema, &schema, NULL,
                      &descriptor, &cursor) == 0);
    CHECK(jdv_stream_pull(&cursor, &record) == JDV_PULL_ITEM);
    CHECK(jdv_stream_pull(&cursor, &record) == JDV_PULL_ERROR &&
          errno == EOVERFLOW);
    CHECK(record.terminal->transform_status == EOVERFLOW);
    CHECK(jdv_stream_release(&cursor) == 0);
    return 0;
}

static int test_copy_reopen_release_and_reentrancy(void) {
    jdv_schema_t schema;
    CHECK(jdv_schema_scalar(JDV_U64, 0, &schema) == 0);
    jdv_value_t value = jdv_value_u64(1);
    jdv_stream_end_t end;
    CHECK(complete_end(&end, 1, 1, &schema) == 0);
    const step_t steps[] = {
        {JDV_PULL_ITEM, &value, NULL, 1, 1, 0},
        {JDV_PULL_END, NULL, &end, 0, 0, 0},
    };
    producer_t producer = {
        .steps = steps, .step_count = 2, .reenter = 1
    };
    jdv_stream_descriptor_t descriptor;
    jdv_stream_cursor_t cursor;
    CHECK(open_stream(&producer, &schema, &schema, NULL,
                      &descriptor, &cursor) == 0);

    jdv_stream_cursor_t copied_cursor = cursor;
    jdv_stream_record_t record;
    CHECK(jdv_stream_pull(&copied_cursor, &record) == JDV_PULL_ERROR &&
          errno == ESTALE);
    jdv_stream_descriptor_t copied_descriptor = descriptor;
    jdv_stream_cursor_t other_cursor;
    CHECK(jdv_stream_open(&copied_descriptor, &other_cursor) < 0 &&
          errno == ESTALE);
    CHECK(jdv_stream_open(&descriptor, &other_cursor) < 0 &&
          errno == EALREADY);

    CHECK(jdv_stream_pull(&cursor, &record) == JDV_PULL_ITEM);
    CHECK(producer.reenter_pull_errno == EBUSY);
    CHECK(producer.reenter_cancel_errno == EBUSY);
    CHECK(producer.reenter_release_errno == EBUSY);
    CHECK(jdv_stream_pull(&cursor, &record) == JDV_PULL_END);
    producer.reenter = 1;
    CHECK(jdv_stream_release(&cursor) == 0);
    CHECK(producer.releases == 1 && producer.reenter_release_errno == EBUSY);
    CHECK(jdv_stream_release(&cursor) == 0);
    CHECK(producer.releases == 1);
    return 0;
}

static int test_old_snapshot_rejected_after_pull(void) {
    jdv_schema_t schema;
    CHECK(jdv_schema_scalar(JDV_U64, 0, &schema) == 0);
    jdv_value_t value = jdv_value_u64(1);
    const step_t steps[] = {
        {JDV_PULL_PENDING, NULL, NULL, 0, 0, 0},
        {JDV_PULL_ITEM, &value, NULL, 1, 1, 0},
    };
    producer_t producer = {.steps = steps, .step_count = 2};
    jdv_stream_descriptor_t descriptor;
    jdv_stream_cursor_t cursor;
    CHECK(open_stream(&producer, &schema, &schema, NULL,
                      &descriptor, &cursor) == 0);

    jdv_stream_cursor_t before_pending = cursor;
    jdv_stream_record_t record;
    CHECK(jdv_stream_pull(&cursor, &record) == JDV_PULL_PENDING);
    jdv_stream_cursor_t after_pending = cursor;
    cursor = before_pending;
    CHECK(jdv_stream_pull(&cursor, &record) == JDV_PULL_ERROR &&
          errno == ESTALE);
    CHECK(jdv_stream_cancel(&cursor, JDV_LITERAL("stale"), NULL) < 0 &&
          errno == ESTALE);
    CHECK(jdv_stream_release(&cursor) < 0 && errno == ESTALE);

    cursor = after_pending;
    jdv_stream_cursor_t before_item = cursor;
    CHECK(jdv_stream_pull(&cursor, &record) == JDV_PULL_ITEM);
    CHECK(jdv_stream_item_valid(&cursor, &record));
    jdv_stream_cursor_t after_item = cursor;
    cursor = before_item;
    CHECK(!jdv_stream_item_valid(&cursor, &record));
    CHECK(jdv_stream_pull(&cursor, &record) == JDV_PULL_ERROR &&
          errno == ESTALE);

    cursor = after_item;
    CHECK(jdv_stream_release(&cursor) == 0);
    CHECK(producer.cancels == 1 && producer.releases == 1);
    return 0;
}

static int test_schema_terminal_mismatch(void) {
    jdv_schema_t number, boolean;
    CHECK(jdv_schema_scalar(JDV_U64, 0, &number) == 0);
    CHECK(jdv_schema_scalar(JDV_BOOL, 0, &boolean) == 0);
    jdv_stream_end_t end;
    CHECK(complete_end(&end, 0, 0, &boolean) == 0);
    const step_t steps[] = {{JDV_PULL_END, NULL, &end, 0, 0, 0}};
    producer_t producer = {.steps = steps, .step_count = 1};
    jdv_stream_descriptor_t descriptor;
    jdv_stream_cursor_t cursor;
    CHECK(open_stream(&producer, &number, &number, NULL,
                      &descriptor, &cursor) == 0);
    jdv_stream_record_t record;
    CHECK(jdv_stream_pull(&cursor, &record) == JDV_PULL_ERROR &&
          errno == EDOM);
    CHECK(record.terminal->state == JDV_STREAM_FAILED &&
          record.terminal->transform_status == EDOM);
    const jdv_schema_set_t* schemas;
    CHECK(jdv_stream_schemas(&cursor, &schemas) == 0);
    CHECK(schemas->validation_scope == JDV_VALIDATION_NONE &&
          schemas->final_schema == NULL);
    CHECK(jdv_stream_release(&cursor) == 0);
    return 0;
}

static int test_borrowed_lifetime(void) {
    jdv_schema_t schema;
    CHECK(jdv_schema_scalar(JDV_U64, 0, &schema) == 0);
    producer_t producer = {0};
    producer.borrowed = jdv_value_u64(9);
    const step_t steps[] = {
        {JDV_PULL_ITEM, &producer.borrowed, NULL, 1, 1, 0},
        {JDV_PULL_PENDING, NULL, NULL, 0, 0, 0},
    };
    producer.steps = steps;
    producer.step_count = 2;
    jdv_stream_descriptor_t descriptor;
    jdv_stream_cursor_t cursor;
    CHECK(open_stream(&producer, &schema, &schema, NULL,
                      &descriptor, &cursor) == 0);
    jdv_stream_record_t first, pending;
    CHECK(jdv_stream_pull(&cursor, &first) == JDV_PULL_ITEM);
    CHECK(jdv_stream_item_valid(&cursor, &first));
    CHECK(jdv_stream_pull(&cursor, &pending) == JDV_PULL_PENDING);
    CHECK(!jdv_stream_item_valid(&cursor, &first));

    producer.next_step = 0;
    CHECK(jdv_stream_pull(&cursor, &first) == JDV_PULL_ITEM);
    CHECK(jdv_stream_item_valid(&cursor, &first));
    CHECK(jdv_stream_cancel(&cursor, JDV_LITERAL("done"), NULL) == 0);
    CHECK(!jdv_stream_item_valid(&cursor, &first));
    CHECK(producer.borrowed.as.u64 == UINT64_C(0xdead));
    CHECK(jdv_stream_release(&cursor) == 0);
    CHECK(producer.borrowed.as.u64 == UINT64_C(0xbeef));
    return 0;
}

static int test_release_before_terminal(void) {
    jdv_schema_t schema;
    CHECK(jdv_schema_scalar(JDV_U64, 0, &schema) == 0);
    producer_t producer = {0};
    jdv_stream_descriptor_t descriptor;
    jdv_stream_cursor_t cursor;
    CHECK(open_stream(&producer, &schema, &schema, NULL,
                      &descriptor, &cursor) == 0);
    CHECK(jdv_stream_release(&cursor) == 0);
    CHECK(producer.cancels == 1 && producer.releases == 1);
    const jdv_stream_end_t* terminal;
    CHECK(jdv_stream_terminal(&cursor, &terminal) == 0);
    CHECK(terminal->state == JDV_STREAM_CANCELLED);
    const jdv_stream_schema_lifecycle_t* lifecycle;
    CHECK(jdv_stream_schema_lifecycle(&cursor, &lifecycle) == 0);
    CHECK(lifecycle->terminal &&
          lifecycle->terminal_state == JDV_STREAM_CANCELLED);
    CHECK(jdv_stream_release(&cursor) == 0);
    CHECK(producer.cancels == 1 && producer.releases == 1);
    return 0;
}

static int test_original_address_restore_after_release(void) {
    jdv_schema_t schema;
    CHECK(jdv_schema_scalar(JDV_U64, 0, &schema) == 0);
    producer_t* producer = calloc(1, sizeof(*producer));
    CHECK(producer != NULL);
    producer->borrowed = jdv_value_u64(42);
    const step_t step = {
        JDV_PULL_ITEM, &producer->borrowed, NULL, 1, 1, 0
    };
    producer->steps = &step;
    producer->step_count = 1;
    jdv_stream_descriptor_t descriptor;
    jdv_stream_cursor_t cursor;
    CHECK(open_stream(producer, &schema, &schema, NULL,
                      &descriptor, &cursor) == 0);
    jdv_stream_record_t borrowed;
    CHECK(jdv_stream_pull(&cursor, &borrowed) == JDV_PULL_ITEM);
    CHECK(jdv_stream_item_valid(&cursor, &borrowed));
    jdv_stream_cursor_t saved = cursor;
    CHECK(jdv_stream_release(&cursor) == 0);
    jdv_stream_cursor_t released = cursor;
    CHECK(descriptor.producer_context == NULL && descriptor.producer == NULL);
    CHECK(producer->cancels == 1 && producer->releases == 1);
    jdv_stream_record_t record;
    CHECK(jdv_stream_pull(&cursor, &record) == JDV_PULL_ERROR &&
          errno == EALREADY);
    CHECK(jdv_stream_cancel(&cursor, JDV_LITERAL("late"), NULL) < 0 &&
          errno == EALREADY);
    free(producer);

    cursor = saved;
    CHECK(!jdv_stream_item_valid(&cursor, &borrowed));
    CHECK(jdv_stream_pull(&cursor, &record) == JDV_PULL_ERROR &&
           errno == ESTALE);
    CHECK(jdv_stream_cancel(&cursor, JDV_LITERAL("late"), NULL) < 0 &&
          errno == ESTALE);
    CHECK(jdv_stream_release(&cursor) < 0 && errno == ESTALE);
    cursor = released;
    CHECK(jdv_stream_release(&cursor) == 0);
    return 0;
}

static int test_authoritative_accounting_and_limits(void) {
    jdv_schema_t schema;
    CHECK(jdv_schema_scalar(JDV_U64, 0, &schema) == 0);
    jdv_value_t value = jdv_value_u64(1);
    const step_t step = {JDV_PULL_ITEM, &value, NULL, 2, 0, 0};
    producer_t producer = {.steps = &step, .step_count = 1};
    jdv_stream_limits_t limits;
    jdv_stream_limits_init(&limits);
    limits.max_logical_bytes = 1;
    jdv_stream_descriptor_t descriptor;
    jdv_stream_cursor_t cursor;
    CHECK(open_stream(&producer, &schema, &schema, &limits,
                      &descriptor, &cursor) == 0);

    descriptor.stream_limits.max_logical_bytes = UINT64_MAX;
    cursor.accounting.logical_bytes = UINT64_MAX;
    jdv_stream_record_t record;
    CHECK(jdv_stream_pull(&cursor, &record) == JDV_PULL_ERROR &&
          errno == E2BIG);
    CHECK(record.terminal->transform_status == E2BIG);
    jdv_stream_accounting_t accounting;
    CHECK(jdv_stream_accounting(&cursor, &accounting) == 0);
    CHECK(accounting.logical_bytes == 0 && accounting.items == 0);
    CHECK(descriptor.stream_limits.max_logical_bytes == 1);
    CHECK(jdv_stream_release(&cursor) == 0);
    return 0;
}

static int test_tiny_value_budget_release(void) {
    jdv_schema_t schema;
    CHECK(jdv_schema_scalar(JDV_U64, 0, &schema) == 0);
    producer_t producer = {0};
    jdv_stream_config_t config;
    jdv_stream_config_init(&config);
    config.declared_schema = &schema;
    config.guaranteed_schema = &schema;
    config.value_limits.max_aggregate_bytes = 0;
    jdv_stream_descriptor_t descriptor;
    jdv_stream_cursor_t cursor;
    CHECK(jdv_stream_descriptor_init(&descriptor, &config, &VTABLE,
                                     &producer) == 0);
    CHECK(jdv_stream_open(&descriptor, &cursor) == 0);
    producer.cursor = &cursor;
    descriptor.value_limits.max_aggregate_bytes = SIZE_MAX;
    CHECK(jdv_stream_cancel(&cursor, JDV_LITERAL("too large"), NULL) < 0 &&
          errno == E2BIG);
    CHECK(producer.cancels == 0 && producer.releases == 0);
    CHECK(jdv_stream_release(&cursor) == 0);
    CHECK(producer.cancels == 1 && producer.releases == 1);
    const jdv_stream_end_t* terminal;
    CHECK(jdv_stream_terminal(&cursor, &terminal) == 0);
    CHECK(terminal->state == JDV_STREAM_CANCELLED &&
          terminal->reason.len == 0);
    CHECK(jdv_stream_release(&cursor) == 0);
    CHECK(producer.cancels == 1 && producer.releases == 1);
    return 0;
}

static int test_runtime_schema_cannot_bypass_declared(void) {
    jdv_schema_t number, unknown, any;
    CHECK(jdv_schema_scalar(JDV_U64, 0, &number) == 0);
    CHECK(jdv_schema_unknown(&unknown) == 0);
    CHECK(jdv_schema_any(&any) == 0);
    jdv_value_t number_value = jdv_value_u64(1);
    jdv_value_t boolean = jdv_value_bool(1);
    const step_t steps[] = {
        {JDV_PULL_ITEM, &number_value, NULL, 1, 1, 0},
        {JDV_PULL_ITEM, &boolean, NULL, 1, 1, 0},
    };
    producer_t producer = {.steps = steps, .step_count = 2};
    jdv_stream_descriptor_t descriptor;
    jdv_stream_cursor_t cursor;
    CHECK(open_stream(&producer, &number, &unknown, NULL,
                      &descriptor, &cursor) == 0);
    descriptor.schemas.declared_schema = &unknown;
    descriptor.value_limits.max_aggregate_bytes = SIZE_MAX;
    jdv_stream_record_t record;
    CHECK(jdv_stream_pull(&cursor, &record) == JDV_PULL_ITEM);
    CHECK(record.item == &number_value);
    CHECK(jdv_stream_pull(&cursor, &record) == JDV_PULL_ERROR &&
          errno == EDOM);
    CHECK(record.terminal->transform_status == EDOM);
    jdv_stream_accounting_t accounting;
    CHECK(jdv_stream_accounting(&cursor, &accounting) == 0);
    CHECK(accounting.items == 1 && accounting.logical_bytes == 1);
    CHECK(jdv_stream_release(&cursor) == 0);

    const step_t unresolved_step = {
        JDV_PULL_ITEM, &number_value, NULL, 1, 1, 0
    };
    producer_t unresolved = {
        .steps = &unresolved_step, .step_count = 1
    };
    CHECK(open_stream(&unresolved, &unknown, &any, NULL,
                      &descriptor, &cursor) == 0);
    CHECK(jdv_stream_pull(&cursor, &record) == JDV_PULL_ERROR &&
          errno == EDOM);
    CHECK(record.terminal->transform_status == EDOM);
    CHECK(jdv_stream_release(&cursor) == 0);
    return 0;
}

static int test_complete_unknown_final_rejected(void) {
    jdv_schema_t number, unknown;
    CHECK(jdv_schema_scalar(JDV_U64, 0, &number) == 0);
    CHECK(jdv_schema_unknown(&unknown) == 0);
    jdv_stream_end_t end;
    CHECK(complete_end(&end, 0, 0, &unknown) == 0);
    const step_t step = {JDV_PULL_END, NULL, &end, 0, 0, 0};
    producer_t producer = {.steps = &step, .step_count = 1};
    jdv_stream_descriptor_t descriptor;
    jdv_stream_cursor_t cursor;
    CHECK(open_stream(&producer, &number, &number, NULL,
                      &descriptor, &cursor) == 0);
    jdv_stream_record_t record;
    CHECK(jdv_stream_pull(&cursor, &record) == JDV_PULL_ERROR &&
          errno == EDOM);
    CHECK(record.terminal->transform_status == EDOM);
    const jdv_stream_schema_lifecycle_t* lifecycle;
    CHECK(jdv_stream_schema_lifecycle(&cursor, &lifecycle) == 0);
    CHECK(lifecycle->terminal &&
          lifecycle->terminal_state == JDV_STREAM_FAILED);
    CHECK(jdv_stream_release(&cursor) == 0);
    return 0;
}

static int test_pending_deltas_rejected(void) {
    jdv_schema_t schema;
    CHECK(jdv_schema_scalar(JDV_U64, 0, &schema) == 0);
    const step_t step = {JDV_PULL_PENDING, NULL, NULL, 0, 1, 0};
    producer_t producer = {.steps = &step, .step_count = 1};
    jdv_stream_descriptor_t descriptor;
    jdv_stream_cursor_t cursor;
    CHECK(open_stream(&producer, &schema, &schema, NULL,
                      &descriptor, &cursor) == 0);
    jdv_stream_record_t record;
    CHECK(jdv_stream_pull(&cursor, &record) == JDV_PULL_ERROR &&
          errno == EPROTO);
    CHECK(record.terminal->producer_status == EPROTO);
    jdv_stream_accounting_t accounting;
    CHECK(jdv_stream_accounting(&cursor, &accounting) == 0);
    CHECK(accounting.physical_bytes == 0);
    CHECK(jdv_stream_release(&cursor) == 0);
    return 0;
}

int main(void) {
    CHECK(test_normal_completion() == 0);
    CHECK(test_empty_and_duplicate_end() == 0);
    CHECK(test_cancellation() == 0);
    CHECK(test_producer_failure_and_pending() == 0);
    CHECK(test_limits_and_overflow() == 0);
    CHECK(test_copy_reopen_release_and_reentrancy() == 0);
    CHECK(test_old_snapshot_rejected_after_pull() == 0);
    CHECK(test_schema_terminal_mismatch() == 0);
    CHECK(test_borrowed_lifetime() == 0);
    CHECK(test_release_before_terminal() == 0);
    CHECK(test_original_address_restore_after_release() == 0);
    CHECK(test_authoritative_accounting_and_limits() == 0);
    CHECK(test_tiny_value_budget_release() == 0);
    CHECK(test_runtime_schema_cannot_bypass_declared() == 0);
    CHECK(test_complete_unknown_final_rejected() == 0);
    CHECK(test_pending_deltas_rejected() == 0);
    puts("data_stream: ok");
    return 0;
}
