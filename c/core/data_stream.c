// SPDX-License-Identifier: LicenseRef-SPL-1.0
#include <core/data_stream.h>

#include <errno.h>
#include <stdint.h>
#include <string.h>

enum {
    DESCRIPTOR_READY = 1,
    CURSOR_ACTIVE = 2,
    CURSOR_TERMINAL = 3,
    CURSOR_RELEASED = 4
};

enum {
    FAILURE_PRODUCER = 1,
    FAILURE_TRANSFORM = 2
};

static int fail_errno(int error) {
    errno = error;
    return -1;
}

static void record_init(jdv_stream_record_t* record) {
    memset(record, 0, sizeof(*record));
    record->version = JDV_STREAM_RECORD_VERSION;
    record->struct_size = (uint32_t)sizeof(*record);
}

void jdv_stream_limits_init(jdv_stream_limits_t* limits) {
    if (!limits) return;
    memset(limits, 0, sizeof(*limits));
    limits->version = JDV_STREAM_LIMITS_VERSION;
    limits->struct_size = (uint32_t)sizeof(*limits);
    limits->max_items = UINT64_MAX;
    limits->max_logical_bytes = UINT64_MAX;
    limits->max_physical_bytes = UINT64_MAX;
    limits->max_materialized_bytes = UINT64_MAX;
}

void jdv_stream_config_init(jdv_stream_config_t* config) {
    if (!config) return;
    memset(config, 0, sizeof(*config));
    config->version = JDV_STREAM_CONFIG_VERSION;
    config->struct_size = (uint32_t)sizeof(*config);
    config->cardinality = JDV_DATA_SEQUENCE;
    config->boundedness = JDV_STREAM_BOUNDED_UNKNOWN;
    config->ordering = JDV_STREAM_ORDERED;
    config->value_limits = JDV_DEFAULT_LIMITS;
    jdv_stream_limits_init(&config->stream_limits);
}

static int stream_limits_valid(const jdv_stream_limits_t* limits) {
    if (!limits) return fail_errno(EINVAL);
    if (limits->version != JDV_STREAM_LIMITS_VERSION ||
        limits->struct_size != sizeof(*limits))
        return fail_errno(EPROTONOSUPPORT);
    return 0;
}

static int producer_valid(const jdv_stream_producer_vtable_t* producer) {
    if (!producer) return fail_errno(EINVAL);
    if (producer->version != JDV_STREAM_VTABLE_VERSION ||
        producer->struct_size != sizeof(*producer))
        return fail_errno(EPROTONOSUPPORT);
    if (!producer->pull || !producer->cancel || !producer->release)
        return fail_errno(EINVAL);
    return 0;
}

static void lifecycle_init(jdv_stream_schema_lifecycle_t* lifecycle,
                           jdv_schema_set_t schemas) {
    memset(lifecycle, 0, sizeof(*lifecycle));
    lifecycle->version = JDV_STREAM_SCHEMA_LIFECYCLE_VERSION;
    lifecycle->struct_size = (uint32_t)sizeof(*lifecycle);
    lifecycle->set = schemas;
    lifecycle->observed_prefix_valid = 1;
    lifecycle->terminal_state = JDV_STREAM_INCOMPLETE;
}

static void sync_descriptor(jdv_stream_descriptor_t* descriptor) {
    descriptor->cardinality = descriptor->_authority_cardinality;
    descriptor->ownership = descriptor->_authority_ownership;
    descriptor->boundedness = descriptor->_authority_boundedness;
    descriptor->ordering = descriptor->_authority_ordering;
    descriptor->provenance_id = descriptor->_authority_provenance_id;
    descriptor->schemas = descriptor->_authority_schema.set;
    descriptor->value_limits = descriptor->_authority_value_limits;
    descriptor->stream_limits = descriptor->_authority_stream_limits;
    descriptor->producer = descriptor->_authority_producer;
    descriptor->producer_context = descriptor->_authority_context;
    descriptor->_runtime_generation = descriptor->_authority_epoch;
    descriptor->_runtime_state = descriptor->_authority_state;
}

static void sync_cursor(jdv_stream_descriptor_t* descriptor,
                        jdv_stream_cursor_t* cursor) {
    sync_descriptor(descriptor);
    if (!cursor) return;
    cursor->accounting = descriptor->_authority_accounting;
    cursor->terminal = descriptor->_authority_terminal;
    cursor->_runtime_generation = descriptor->_authority_epoch;
    cursor->_runtime_borrow_generation =
        descriptor->_authority_borrow_generation;
    cursor->_runtime_state = descriptor->_authority_state;
    cursor->_runtime_busy = descriptor->_authority_busy;
    cursor->_runtime_borrow_active = descriptor->_authority_borrow_active;
    cursor->_runtime_terminal_observed = descriptor->_authority_schema.terminal;
}

static int descriptor_valid(const jdv_stream_descriptor_t* descriptor) {
    if (!descriptor) return fail_errno(EINVAL);
    if (descriptor->version != JDV_STREAM_DESCRIPTOR_VERSION ||
        descriptor->struct_size != sizeof(*descriptor))
        return fail_errno(EPROTONOSUPPORT);
    if (descriptor->_runtime_self != descriptor) return fail_errno(ESTALE);
    if (descriptor->_authority_cardinality != JDV_DATA_SEQUENCE &&
        descriptor->_authority_cardinality != JDV_DATA_BYTE_STREAM)
        return fail_errno(EINVAL);
    if (descriptor->_authority_ownership != JDV_OWNERSHIP_LINEAR_STREAM ||
        descriptor->_authority_boundedness < JDV_STREAM_BOUNDED_UNKNOWN ||
        descriptor->_authority_boundedness > JDV_STREAM_UNBOUNDED ||
        descriptor->_authority_ordering < JDV_STREAM_ORDERED ||
        descriptor->_authority_ordering > JDV_STREAM_PARTIALLY_ORDERED)
        return fail_errno(EINVAL);
    if (descriptor->_authority_state < DESCRIPTOR_READY ||
        descriptor->_authority_state > CURSOR_RELEASED)
        return fail_errno(EINVAL);
    if (stream_limits_valid(&descriptor->_authority_stream_limits) < 0)
        return -1;
    if (descriptor->_authority_state == CURSOR_RELEASED) {
        if (descriptor->_authority_producer || descriptor->_authority_context ||
            !descriptor->_authority_release_called)
            return fail_errno(EINVAL);
    } else if (producer_valid(descriptor->_authority_producer) < 0) {
        return -1;
    }
    if (descriptor->_authority_schema.version !=
            JDV_STREAM_SCHEMA_LIFECYCLE_VERSION ||
        descriptor->_authority_schema.struct_size !=
            sizeof(descriptor->_authority_schema) ||
        !descriptor->_authority_schema.set.declared_schema ||
        !descriptor->_authority_schema.set.guaranteed_schema)
        return fail_errno(EPROTONOSUPPORT);
    if (descriptor->_authority_accounting.version !=
            JDV_STREAM_ACCOUNTING_VERSION ||
        descriptor->_authority_accounting.struct_size !=
            sizeof(descriptor->_authority_accounting))
        return fail_errno(EPROTONOSUPPORT);

    jdv_relation_t relation;
    if (jdv_schema_relation_checked(
            descriptor->_authority_schema.set.guaranteed_schema,
            descriptor->_authority_schema.set.declared_schema,
            &relation, &descriptor->_authority_value_limits) < 0)
        return -1;
    if (relation == JDV_RELATION_NO ||
        descriptor->_authority_relation_runtime !=
            (relation == JDV_RELATION_RUNTIME))
        return fail_errno(EDOM);
    return 0;
}

int jdv_stream_descriptor_init(jdv_stream_descriptor_t* descriptor,
                               const jdv_stream_config_t* config,
                               const jdv_stream_producer_vtable_t* producer,
                               void* producer_context) {
    if (!descriptor || !config) return fail_errno(EINVAL);
    if (config->version != JDV_STREAM_CONFIG_VERSION ||
        config->struct_size != sizeof(*config))
        return fail_errno(EPROTONOSUPPORT);
    if (config->cardinality != JDV_DATA_SEQUENCE &&
        config->cardinality != JDV_DATA_BYTE_STREAM)
        return fail_errno(EINVAL);
    if (config->boundedness < JDV_STREAM_BOUNDED_UNKNOWN ||
        config->boundedness > JDV_STREAM_UNBOUNDED ||
        config->ordering < JDV_STREAM_ORDERED ||
        config->ordering > JDV_STREAM_PARTIALLY_ORDERED)
        return fail_errno(EINVAL);
    if (stream_limits_valid(&config->stream_limits) < 0 ||
        producer_valid(producer) < 0)
        return -1;

    jdv_schema_set_t schemas;
    if (jdv_schema_set_init(&schemas, config->declared_schema,
                            config->guaranteed_schema,
                            &config->value_limits) < 0)
        return -1;
    jdv_relation_t relation;
    if (jdv_schema_relation_checked(config->guaranteed_schema,
                                    config->declared_schema, &relation,
                                    &config->value_limits) < 0)
        return -1;
    if (relation == JDV_RELATION_NO) return fail_errno(EDOM);

    jdv_stream_descriptor_t result;
    memset(&result, 0, sizeof(result));
    result.version = JDV_STREAM_DESCRIPTOR_VERSION;
    result.struct_size = (uint32_t)sizeof(result);
    result._runtime_self = descriptor;
    result._authority_cardinality = config->cardinality;
    result._authority_ownership = JDV_OWNERSHIP_LINEAR_STREAM;
    result._authority_boundedness = config->boundedness;
    result._authority_ordering = config->ordering;
    result._authority_provenance_id = config->provenance_id;
    lifecycle_init(&result._authority_schema, schemas);
    result._authority_value_limits = config->value_limits;
    result._authority_stream_limits = config->stream_limits;
    result._authority_accounting.version = JDV_STREAM_ACCOUNTING_VERSION;
    result._authority_accounting.struct_size =
        (uint32_t)sizeof(result._authority_accounting);
    if (jdv_stream_end_init(&result._authority_terminal) < 0) return -1;
    result._authority_producer = producer;
    result._authority_context = producer_context;
    result._authority_epoch = 1;
    result._authority_borrow_generation = 1;
    result._authority_state = DESCRIPTOR_READY;
    result._authority_relation_runtime = relation == JDV_RELATION_RUNTIME;
    *descriptor = result;
    sync_descriptor(descriptor);
    return 0;
}

static int cursor_header_valid(const jdv_stream_cursor_t* cursor) {
    if (!cursor) return fail_errno(EINVAL);
    if (cursor->version != JDV_STREAM_CURSOR_VERSION ||
        cursor->struct_size != sizeof(*cursor))
        return fail_errno(EPROTONOSUPPORT);
    if (cursor->_runtime_self != cursor) return fail_errno(ESTALE);
    return 0;
}

static int cursor_valid(const jdv_stream_cursor_t* cursor) {
    if (cursor_header_valid(cursor) < 0 ||
        descriptor_valid(cursor->descriptor) < 0)
        return -1;
    const jdv_stream_descriptor_t* descriptor = cursor->descriptor;
    if (descriptor->_runtime_cursor != cursor ||
        cursor->_runtime_generation != descriptor->_authority_epoch)
        return fail_errno(ESTALE);
    return 0;
}

int jdv_stream_open(jdv_stream_descriptor_t* descriptor,
                    jdv_stream_cursor_t* cursor) {
    if (!cursor || descriptor_valid(descriptor) < 0) return -1;
    if (descriptor->_authority_state != DESCRIPTOR_READY)
        return fail_errno(EALREADY);
    if (descriptor->_authority_epoch == UINT64_MAX)
        return fail_errno(EOVERFLOW);

    jdv_stream_cursor_t result;
    memset(&result, 0, sizeof(result));
    result.version = JDV_STREAM_CURSOR_VERSION;
    result.struct_size = (uint32_t)sizeof(result);
    result.descriptor = descriptor;
    result._runtime_self = cursor;
    descriptor->_authority_epoch++;
    descriptor->_authority_state = CURSOR_ACTIVE;
    descriptor->_runtime_cursor = cursor;
    *cursor = result;
    sync_cursor(descriptor, cursor);
    return 0;
}

static int cursor_available(jdv_stream_cursor_t* cursor) {
    if (cursor_valid(cursor) < 0) return -1;
    jdv_stream_descriptor_t* descriptor = cursor->descriptor;
    if (descriptor->_authority_busy) return fail_errno(EBUSY);
    if (descriptor->_authority_state != CURSOR_ACTIVE)
        return fail_errno(EALREADY);
    return 0;
}

static int advance_pull_epoch(jdv_stream_descriptor_t* descriptor,
                              jdv_stream_cursor_t* cursor) {
    if (descriptor->_authority_epoch == UINT64_MAX)
        return fail_errno(EOVERFLOW);
    descriptor->_authority_epoch++;
    sync_cursor(descriptor, cursor);
    return 0;
}

static int invalidate_borrow(jdv_stream_descriptor_t* descriptor) {
    descriptor->_authority_borrow_active = 0;
    if (descriptor->_authority_borrow_generation == UINT64_MAX)
        return fail_errno(EOVERFLOW);
    descriptor->_authority_borrow_generation++;
    return 0;
}

static int add_checked(uint64_t current, uint64_t amount, uint64_t maximum,
                       uint64_t* result) {
    if (amount > UINT64_MAX - current) return fail_errno(EOVERFLOW);
    if (current > maximum || amount > maximum - current)
        return fail_errno(E2BIG);
    *result = current + amount;
    return 0;
}

static int account_record(jdv_stream_descriptor_t* descriptor,
                          const jdv_stream_record_t* record,
                          int has_item) {
    jdv_stream_accounting_t next = descriptor->_authority_accounting;
    const jdv_stream_limits_t* limits =
        &descriptor->_authority_stream_limits;
    if (add_checked(next.items, has_item ? 1u : 0u, limits->max_items,
                    &next.items) < 0 ||
        add_checked(next.logical_bytes, record->logical_bytes,
                    limits->max_logical_bytes, &next.logical_bytes) < 0 ||
        add_checked(next.physical_bytes, record->physical_bytes,
                    limits->max_physical_bytes, &next.physical_bytes) < 0 ||
        add_checked(next.materialized_bytes, record->materialized_bytes,
                    limits->max_materialized_bytes,
                    &next.materialized_bytes) < 0)
        return -1;
    descriptor->_authority_accounting = next;
    return 0;
}

static void mark_schema_terminal(jdv_stream_schema_lifecycle_t* lifecycle,
                                 jdv_stream_state_t state) {
    lifecycle->terminal = 1;
    lifecycle->terminal_state = state;
}

static jdv_stream_pull_result_t failure_terminal(
    jdv_stream_cursor_t* cursor, jdv_stream_record_t* record, int error,
    int stage, const char* reason) {
    jdv_stream_descriptor_t* descriptor = cursor->descriptor;
    jdv_stream_end_t end;
    (void)jdv_stream_end_init(&end);
    end.state = JDV_STREAM_FAILED;
    end.items_seen = descriptor->_authority_accounting.items;
    end.bytes_seen = descriptor->_authority_accounting.logical_bytes;
    end.errors_seen = 1;
    end.completeness = (end.items_seen || end.bytes_seen)
        ? JDV_COMPLETENESS_PARTIAL : JDV_COMPLETENESS_UNKNOWN;
    end.reason.data = (const uint8_t*)reason;
    end.reason.len = strlen(reason);
    if (stage == FAILURE_PRODUCER) end.producer_status = error;
    if (stage == FAILURE_TRANSFORM) end.transform_status = error;
    descriptor->_authority_terminal = end;
    descriptor->_authority_state = CURSOR_TERMINAL;
    descriptor->_authority_borrow_active = 0;
    mark_schema_terminal(&descriptor->_authority_schema, end.state);
    sync_cursor(descriptor, cursor);
    record_init(record);
    record->terminal = &descriptor->_authority_terminal;
    errno = error;
    return JDV_PULL_ERROR;
}

static int final_schema_valid(jdv_stream_descriptor_t* descriptor,
                              const jdv_schema_t* final_schema,
                              int complete) {
    if (!final_schema) return fail_errno(EINVAL);
    jdv_relation_t declared;
    jdv_relation_t guaranteed;
    const jdv_schema_set_t* set = &descriptor->_authority_schema.set;
    if (jdv_schema_relation_checked(final_schema, set->declared_schema,
                                    &declared,
                                    &descriptor->_authority_value_limits) < 0 ||
        jdv_schema_relation_checked(final_schema, set->guaranteed_schema,
                                    &guaranteed,
                                    &descriptor->_authority_value_limits) < 0)
        return -1;
    if ((complete && declared != JDV_RELATION_YES) ||
        (!complete && declared == JDV_RELATION_NO) ||
        guaranteed == JDV_RELATION_NO ||
        (complete && !descriptor->_authority_relation_runtime &&
         guaranteed != JDV_RELATION_YES))
        return fail_errno(EDOM);
    return 0;
}

static int terminal_schema_transition(
    jdv_stream_descriptor_t* descriptor, const jdv_stream_end_t* terminal,
    jdv_stream_schema_lifecycle_t* result) {
    *result = descriptor->_authority_schema;
    if (terminal->state == JDV_STREAM_COMPLETE) {
        if (jdv_schema_set_transition(&result->set, terminal->final_schema,
                                      terminal->final_schema,
                                      JDV_VALIDATION_COMPLETE,
                                      &descriptor->_authority_value_limits) < 0)
            return -1;
    } else if (terminal->final_schema) {
        if (jdv_schema_set_transition(&result->set, terminal->final_schema,
                                      terminal->final_schema,
                                      JDV_VALIDATION_FAILED,
                                      &descriptor->_authority_value_limits) < 0)
            return -1;
    }
    mark_schema_terminal(result, terminal->state);
    return 0;
}

static int accept_terminal(jdv_stream_descriptor_t* descriptor,
                           const jdv_stream_end_t* terminal,
                           jdv_stream_pull_result_t result) {
    if (jdv_stream_end_validate(
            terminal, &descriptor->_authority_value_limits) < 0)
        return -1;
    if ((result == JDV_PULL_END && terminal->state != JDV_STREAM_COMPLETE) ||
        (result == JDV_PULL_ERROR &&
          terminal->state != JDV_STREAM_CANCELLED &&
          terminal->state != JDV_STREAM_FAILED &&
          terminal->state != JDV_STREAM_INCOMPLETE))
        return fail_errno(EPROTO);
    if (terminal->items_seen != descriptor->_authority_accounting.items ||
        terminal->bytes_seen !=
            descriptor->_authority_accounting.logical_bytes)
        return fail_errno(EBADMSG);
    if (terminal->final_schema &&
        final_schema_valid(descriptor, terminal->final_schema,
                           terminal->state == JDV_STREAM_COMPLETE) < 0)
        return -1;

    jdv_stream_schema_lifecycle_t lifecycle;
    if (terminal_schema_transition(descriptor, terminal, &lifecycle) < 0)
        return -1;
    descriptor->_authority_schema = lifecycle;
    descriptor->_authority_terminal = *terminal;
    descriptor->_authority_state = CURSOR_TERMINAL;
    descriptor->_authority_borrow_active = 0;
    return 0;
}

static int validate_item(jdv_stream_descriptor_t* descriptor,
                         const jdv_value_t* item,
                         jdv_relation_t* declared,
                         jdv_relation_t* guaranteed) {
    const jdv_schema_set_t* set = &descriptor->_authority_schema.set;
    if (jdv_schema_accepts_checked(set->guaranteed_schema, item, guaranteed,
                                   &descriptor->_authority_value_limits) < 0 ||
        jdv_schema_accepts_checked(set->declared_schema, item, declared,
                                   &descriptor->_authority_value_limits) < 0)
        return -1;
    if (*guaranteed == JDV_RELATION_NO || *declared == JDV_RELATION_NO ||
        (descriptor->_authority_relation_runtime &&
         *declared != JDV_RELATION_YES))
        return fail_errno(EDOM);
    return 0;
}

static int observe_item_schema(jdv_stream_descriptor_t* descriptor,
                               const jdv_value_t* item,
                               jdv_relation_t declared,
                               jdv_relation_t guaranteed) {
    jdv_stream_schema_lifecycle_t next = descriptor->_authority_schema;
    next.observed_items = descriptor->_authority_accounting.items;
    const jdv_schema_t* validated = NULL;
    if (declared == JDV_RELATION_YES)
        validated = next.set.declared_schema;
    else if (guaranteed == JDV_RELATION_YES)
        validated = next.set.guaranteed_schema;

    if (!validated) {
        next.observed_prefix_valid = 0;
    } else if (next.set.validation_scope == JDV_VALIDATION_NONE) {
        if (jdv_schema_set_transition(&next.set, validated, NULL,
                                      JDV_VALIDATION_PREFIX,
                                      &descriptor->_authority_value_limits) < 0)
            return -1;
    } else if (next.observed_prefix_valid) {
        jdv_relation_t existing;
        if (jdv_schema_accepts_checked(next.set.observed_schema, item,
                                       &existing,
                                       &descriptor->_authority_value_limits) < 0)
            return -1;
        if (existing != JDV_RELATION_YES)
            next.observed_prefix_valid = 0;
    }
    descriptor->_authority_schema = next;
    return 0;
}

jdv_stream_pull_result_t jdv_stream_pull(jdv_stream_cursor_t* cursor,
                                         jdv_stream_record_t* record) {
    if (!record) {
        errno = EINVAL;
        return JDV_PULL_ERROR;
    }
    record_init(record);
    if (cursor_available(cursor) < 0) return JDV_PULL_ERROR;
    jdv_stream_descriptor_t* descriptor = cursor->descriptor;
    if (invalidate_borrow(descriptor) < 0)
        return failure_terminal(cursor, record, errno, FAILURE_TRANSFORM,
                                "borrow generation overflow");
    if (advance_pull_epoch(descriptor, cursor) < 0)
        return failure_terminal(cursor, record, errno, FAILURE_TRANSFORM,
                                "stream epoch overflow");

    jdv_stream_record_t produced;
    record_init(&produced);
    descriptor->_authority_busy = 1;
    sync_cursor(descriptor, cursor);
    jdv_stream_pull_result_t result = descriptor->_authority_producer->pull(
        descriptor->_authority_context, &produced);
    descriptor->_authority_busy = 0;

    if (produced.version != JDV_STREAM_RECORD_VERSION ||
        produced.struct_size != sizeof(produced))
        return failure_terminal(cursor, record, EPROTO, FAILURE_PRODUCER,
                                "invalid producer record version");
    if (result < JDV_PULL_ITEM || result > JDV_PULL_ERROR)
        return failure_terminal(cursor, record, EPROTO, FAILURE_PRODUCER,
                                "invalid producer pull result");
    if ((result == JDV_PULL_ITEM &&
         (!produced.item || produced.terminal)) ||
        (result == JDV_PULL_PENDING &&
         (produced.item || produced.terminal)) ||
        ((result == JDV_PULL_END || result == JDV_PULL_ERROR) &&
         (produced.item || !produced.terminal)))
        return failure_terminal(cursor, record, EPROTO, FAILURE_PRODUCER,
                                "invalid producer record shape");
    if (result == JDV_PULL_PENDING &&
        (produced.logical_bytes || produced.physical_bytes ||
         produced.materialized_bytes))
        return failure_terminal(cursor, record, EPROTO, FAILURE_PRODUCER,
                                "pending record has accounting deltas");

    jdv_relation_t declared = JDV_RELATION_RUNTIME;
    jdv_relation_t guaranteed = JDV_RELATION_RUNTIME;
    if (result == JDV_PULL_ITEM &&
        validate_item(descriptor, produced.item, &declared, &guaranteed) < 0)
        return failure_terminal(cursor, record, errno, FAILURE_TRANSFORM,
                                "item violates stream schema");
    if (result != JDV_PULL_PENDING &&
        account_record(descriptor, &produced,
                       result == JDV_PULL_ITEM) < 0)
        return failure_terminal(cursor, record, errno, FAILURE_TRANSFORM,
                                "stream accounting limit");
    if (result == JDV_PULL_ITEM &&
        observe_item_schema(descriptor, produced.item,
                            declared, guaranteed) < 0)
        return failure_terminal(cursor, record, errno, FAILURE_TRANSFORM,
                                "stream schema lifecycle failure");

    *record = produced;
    record->borrow_owner = NULL;
    record->borrow_generation = 0;
    if (result == JDV_PULL_ITEM) {
        descriptor->_authority_borrow_active = 1;
        record->borrow_owner = cursor;
        record->borrow_generation =
            descriptor->_authority_borrow_generation;
        sync_cursor(descriptor, cursor);
        errno = 0;
        return result;
    }
    if (result == JDV_PULL_PENDING) {
        sync_cursor(descriptor, cursor);
        errno = 0;
        return result;
    }
    if (accept_terminal(descriptor, produced.terminal, result) < 0)
        return failure_terminal(cursor, record, errno, FAILURE_TRANSFORM,
                                "invalid stream terminal");
    sync_cursor(descriptor, cursor);
    record->terminal = &descriptor->_authority_terminal;
    errno = 0;
    return result;
}

static void cancellation_terminal(jdv_stream_descriptor_t* descriptor,
                                  jdv_slice_t reason,
                                  int producer_status) {
    jdv_stream_end_t end;
    (void)jdv_stream_end_init(&end);
    end.state = JDV_STREAM_CANCELLED;
    end.items_seen = descriptor->_authority_accounting.items;
    end.bytes_seen = descriptor->_authority_accounting.logical_bytes;
    end.producer_status = producer_status;
    end.completeness = (end.items_seen || end.bytes_seen)
        ? JDV_COMPLETENESS_PARTIAL : JDV_COMPLETENESS_UNKNOWN;
    end.reason = reason;
    descriptor->_authority_terminal = end;
    descriptor->_authority_state = CURSOR_TERMINAL;
    descriptor->_authority_borrow_active = 0;
    mark_schema_terminal(&descriptor->_authority_schema, end.state);
}

int jdv_stream_cancel(jdv_stream_cursor_t* cursor, jdv_slice_t reason,
                      const jdv_stream_end_t** terminal) {
    if (terminal) *terminal = NULL;
    if (cursor_valid(cursor) < 0) return -1;
    jdv_stream_descriptor_t* descriptor = cursor->descriptor;
    if (descriptor->_authority_busy) return fail_errno(EBUSY);
    if (descriptor->_authority_state == CURSOR_RELEASED)
        return fail_errno(EALREADY);
    if (descriptor->_authority_state == CURSOR_TERMINAL) {
        if (descriptor->_authority_terminal.state != JDV_STREAM_CANCELLED)
            return fail_errno(EALREADY);
        if (terminal) *terminal = &descriptor->_authority_terminal;
        errno = 0;
        return 0;
    }

    jdv_stream_end_t check;
    if (jdv_stream_end_init(&check) < 0) return -1;
    check.state = JDV_STREAM_CANCELLED;
    check.completeness = JDV_COMPLETENESS_UNKNOWN;
    check.reason = reason;
    if (jdv_stream_end_validate(
            &check, &descriptor->_authority_value_limits) < 0)
        return -1;
    if (invalidate_borrow(descriptor) < 0) return -1;

    int status = 0;
    if (!descriptor->_authority_cancel_called) {
        descriptor->_authority_cancel_called = 1;
        descriptor->_authority_busy = 1;
        status = descriptor->_authority_producer->cancel(
            descriptor->_authority_context, reason);
        descriptor->_authority_busy = 0;
    }
    cancellation_terminal(descriptor, reason, status);
    sync_cursor(descriptor, cursor);
    if (terminal) *terminal = &descriptor->_authority_terminal;
    errno = 0;
    return 0;
}

int jdv_stream_release(jdv_stream_cursor_t* cursor) {
    if (cursor_header_valid(cursor) < 0 ||
        descriptor_valid(cursor->descriptor) < 0)
        return -1;
    jdv_stream_descriptor_t* descriptor = cursor->descriptor;
    if (descriptor->_runtime_cursor != cursor ||
        cursor->_runtime_generation != descriptor->_authority_epoch)
        return fail_errno(ESTALE);
    if (descriptor->_authority_state == CURSOR_RELEASED) {
        errno = 0;
        return 0;
    }
    if (descriptor->_authority_busy) return fail_errno(EBUSY);

    (void)invalidate_borrow(descriptor);
    if (descriptor->_authority_state == CURSOR_ACTIVE) {
        jdv_slice_t no_reason = {NULL, 0};
        int status = 0;
        if (!descriptor->_authority_cancel_called) {
            descriptor->_authority_cancel_called = 1;
            descriptor->_authority_busy = 1;
            status = descriptor->_authority_producer->cancel(
                descriptor->_authority_context, no_reason);
            descriptor->_authority_busy = 0;
        }
        cancellation_terminal(descriptor, no_reason, status);
    }
    if (!descriptor->_authority_release_called) {
        descriptor->_authority_release_called = 1;
        descriptor->_authority_busy = 1;
        descriptor->_authority_producer->release(
            descriptor->_authority_context);
        descriptor->_authority_busy = 0;
    }
    descriptor->_authority_context = NULL;
    descriptor->_authority_producer = NULL;
    descriptor->_authority_state = CURSOR_RELEASED;
    descriptor->_authority_borrow_active = 0;
    descriptor->_authority_epoch = descriptor->_authority_epoch == UINT64_MAX
        ? 0 : descriptor->_authority_epoch + 1u;
    sync_cursor(descriptor, cursor);
    errno = 0;
    return 0;
}

int jdv_stream_terminal(const jdv_stream_cursor_t* cursor,
                        const jdv_stream_end_t** terminal) {
    if (!terminal) return fail_errno(EINVAL);
    *terminal = NULL;
    if (cursor_valid(cursor) < 0) return -1;
    const jdv_stream_descriptor_t* descriptor = cursor->descriptor;
    if (descriptor->_authority_busy) return fail_errno(EBUSY);
    if (descriptor->_authority_state == CURSOR_ACTIVE)
        return fail_errno(ENODATA);
    *terminal = &descriptor->_authority_terminal;
    return 0;
}

int jdv_stream_accounting(const jdv_stream_cursor_t* cursor,
                          jdv_stream_accounting_t* accounting) {
    if (!accounting) return fail_errno(EINVAL);
    if (cursor_valid(cursor) < 0) return -1;
    const jdv_stream_descriptor_t* descriptor = cursor->descriptor;
    if (descriptor->_authority_busy) return fail_errno(EBUSY);
    *accounting = descriptor->_authority_accounting;
    return 0;
}

int jdv_stream_schemas(const jdv_stream_cursor_t* cursor,
                       const jdv_schema_set_t** schemas) {
    if (!schemas) return fail_errno(EINVAL);
    *schemas = NULL;
    if (cursor_valid(cursor) < 0) return -1;
    const jdv_stream_descriptor_t* descriptor = cursor->descriptor;
    if (descriptor->_authority_busy) return fail_errno(EBUSY);
    *schemas = &descriptor->_authority_schema.set;
    return 0;
}

int jdv_stream_schema_lifecycle(
    const jdv_stream_cursor_t* cursor,
    const jdv_stream_schema_lifecycle_t** lifecycle) {
    if (!lifecycle) return fail_errno(EINVAL);
    *lifecycle = NULL;
    if (cursor_valid(cursor) < 0) return -1;
    const jdv_stream_descriptor_t* descriptor = cursor->descriptor;
    if (descriptor->_authority_busy) return fail_errno(EBUSY);
    *lifecycle = &descriptor->_authority_schema;
    return 0;
}

int jdv_stream_item_valid(const jdv_stream_cursor_t* cursor,
                          const jdv_stream_record_t* record) {
    if (!cursor || !record || cursor->_runtime_self != cursor ||
        cursor->version != JDV_STREAM_CURSOR_VERSION ||
        cursor->struct_size != sizeof(*cursor) ||
        record->version != JDV_STREAM_RECORD_VERSION ||
        record->struct_size != sizeof(*record) || !cursor->descriptor)
        return 0;
    const jdv_stream_descriptor_t* descriptor = cursor->descriptor;
    return descriptor->_runtime_self == descriptor &&
           descriptor->_runtime_cursor == cursor &&
           cursor->_runtime_generation == descriptor->_authority_epoch &&
           descriptor->_authority_state == CURSOR_ACTIVE &&
           !descriptor->_authority_busy &&
           descriptor->_authority_borrow_active && record->item &&
           record->borrow_owner == cursor &&
           record->borrow_generation ==
               descriptor->_authority_borrow_generation;
}
