// SPDX-License-Identifier: LicenseRef-SPL-1.0
#ifndef JASH_BUILTINS_DF_BASELINE_DATA_H
#define JASH_BUILTINS_DF_BASELINE_DATA_H

static jdv_value_t df_baseline_data_item(df_data_row_t* row, const df_baseline_result_t* result) {
    jdv_value_t value = df_data_item(row, NULL, NULL, 0, 0, 0, NULL, 0,
        result->error, result->error, result->error_number);
    row->fields[DF_INDEX].value = jdv_value_nothing();
    row->fields[DF_ROW_KIND].value = query_data_text(JDV_SYMBOL, "baseline");
    row->fields[DF_BASELINE].value = query_data_record(row->baseline, &DF_BASELINE_SCHEMA);
    row->baseline[0].value = query_data_text(JDV_SYMBOL, "baseline_create");
    row->baseline[1].value = jdv_value_filesize(result->bytes_written);
    row->baseline[2].value = query_data_text(JDV_PATH, result->destination);
    row->baseline[3].value = jdv_value_bool(result->durable);
    row->baseline[4].value = query_data_text(JDV_SYMBOL, result->published ? "changed" : "none");
    if (result->error_number) row->baseline[5].value = jdv_value_u64((uint64_t)result->error_number);
    if (result->error) row->baseline[6].value = query_data_text(JDV_SYMBOL, result->error);
    row->baseline[7].value = query_data_text(JDV_SYMBOL,
        result->published ? result->error ? "partial" : "completed" : "failed");
    if (result->undo.token[0]) {
        row->baseline[8].value = query_data_record(row->undo, &DF_UNDO_SCHEMA);
        row->undo[0].value = query_data_text(JDV_SYMBOL, result->undo.coverage);
        row->undo[1].value = jdv_value_u64(result->undo.entry_id);
        row->undo[2].value = query_data_text(JDV_STRING, result->undo.token);
    }
    row->baseline[9].value = jdv_value_bool(result->verified);
    return value;
}

static void df_baseline_json(jash_json_writer_t* json, const df_baseline_result_t* result) {
    jash_jsonw_raw(json, "{\"action\":\"baseline_create\",\"destination\":");
    jash_jsonw_string_or_bytes(json, result->destination);
    jash_jsonw_raw(json, ",\"status\":");
    jash_jsonw_string(json, result->published ? result->error ? "partial" : "completed" : "failed");
    jash_jsonw_raw(json, ",\"effect\":"); jash_jsonw_string(json, result->published ? "changed" : "none");
    jash_jsonw_raw(json, ",\"bytes_written\":"); jash_jsonw_safe_u64(json, result->bytes_written);
    jash_jsonw_raw(json, ",\"durable\":"); jash_jsonw_raw(json, result->durable ? "true" : "false");
    jash_jsonw_raw(json, ",\"verified\":"); jash_jsonw_raw(json, result->verified ? "true" : "false");
    jash_jsonw_raw(json, ",\"errno\":");
    if (result->error_number) jash_jsonw_u64(json, (uint64_t)result->error_number); else jash_jsonw_raw(json, "null");
    jash_jsonw_raw(json, ",\"error\":");
    if (result->error) jash_jsonw_string(json, result->error); else jash_jsonw_raw(json, "null");
    jash_jsonw_raw(json, ",\"undo\":");
    if (result->undo.token[0]) {
        jash_jsonw_raw(json, "{\"token\":"); jash_jsonw_string(json, result->undo.token);
        jash_jsonw_raw(json, ",\"entry_id\":"); jash_jsonw_safe_u64(json, result->undo.entry_id);
        jash_jsonw_raw(json, ",\"coverage\":"); jash_jsonw_string(json, result->undo.coverage);
        jash_jsonw_raw(json, "}");
    } else jash_jsonw_raw(json, "null");
    jash_jsonw_raw(json, "}");
}

static int df_baseline_data_publish(jash_state_t* state, jash_writer_t writer, void* context,
    df_data_row_t* rows, jdv_value_t* items, size_t count, uint64_t errors,
    int truncated, int page_partial, int cancelled, const char* destination,
    const df_fs_info_t* filesystems, int filesystem_count) {
    df_baseline_result_t receipt = {.destination = destination};
    if (!cancelled) cancelled = jash_query_cancel_poll(state);
    if (cancelled || errors || truncated || !filesystem_count) {
        receipt.cancelled = cancelled;
        receipt.error = cancelled ? "request_cancelled" : truncated ? "mount_collection_limit" : "baseline_observation_failed";
        items[count] = df_baseline_data_item(&rows[count], &receipt);
        return query_data_emit(writer, context, &DF_DATA_SCHEMA, items, count + 1u,
            errors + (cancelled ? 0u : 1u), cancelled ? cancelled : truncated ? 75 : 1,
            cancelled ? JDV_STREAM_CANCELLED : JDV_STREAM_FAILED, receipt.error);
    }
    /* Validate the completed record shape, including its longest dynamic
     * fields and recovery reference, before opening any staging artifact. */
    df_baseline_result_t admitted = {.destination = destination,
        .error = "baseline_directory_sync_failed", .bytes_written = UINT64_MAX,
        .published = 1, .error_number = INT_MAX};
    memcpy(admitted.undo.token, "00000000000000000000000000000000", 33);
    admitted.undo.entry_id = UINT64_MAX; admitted.undo.coverage = "guarded_inverse";
    items[count] = df_baseline_data_item(&rows[count], &admitted);
    uint64_t* credits = calloc(count + 1u, sizeof(*credits));
    unsigned char* workspace = malloc(QUERY_DATA_ITEM_WORKSPACE);
    if (!credits || !workspace) {
        free(credits); free(workspace);
        return query_data_error(writer, context, &DF_DATA_SCHEMA, "allocation_failed", 1);
    }
    int invalid = 0;
    for (size_t i = 0; i <= count; i++) {
        jdv_writer_t measure; jdv_writer_init(&measure, query_data_measure, &credits[i]);
        if (jdv_encode(&items[i], &measure) || credits[i] > QUERY_DATA_ITEM_WORKSPACE - 8u) { invalid = 1; break; }
    }
    jdv_stream_config_t config; jdv_stream_config_init(&config);
    config.boundedness = JDV_STREAM_FINITE;
    config.declared_schema = config.guaranteed_schema = &DF_DATA_SCHEMA;
    config.stream_limits.max_items = QUERY_DATA_MAX_ITEMS + 2u;
    config.stream_limits.max_logical_bytes = config.stream_limits.max_physical_bytes =
        config.stream_limits.max_materialized_bytes = QUERY_DATA_MAX_BYTES;
    jdv_stream_end_t outcome, final;
    (void)jdv_stream_end_init(&outcome);
    outcome.state = JDV_STREAM_COMPLETE; outcome.completeness = JDV_COMPLETENESS_COMPLETE;
    uint64_t discarded = 0;
    if (!invalid && jdv_emit_sequence(items, count + 1u, credits, &config, &outcome,
        query_data_measure, &discarded, workspace, QUERY_DATA_ITEM_WORKSPACE, &final)) invalid = 1;
    if (invalid) {
        free(credits); free(workspace);
        return query_data_error(writer, context, &DF_DATA_SCHEMA, "baseline_admission_failed", 1);
    }
    struct sigaction previous, ignore = {0};
    ignore.sa_handler = SIG_IGN; sigemptyset(&ignore.sa_mask);
    if (sigaction(SIGPIPE, &ignore, &previous)) { free(credits); free(workspace); return 125; }
    jash_budget_unwrap(&writer, &context); jash_redir_stream_writer(writer, context);
    query_data_sink_t sink = {writer, context};
    jdv_wire_push_t* stream = NULL;
    int result = 125;
    if (jdv_wire_push_v2_open(&stream, &config, query_data_write, &sink, workspace, QUERY_DATA_ITEM_WORKSPACE)) goto done;
    for (size_t i = 0; i < count; i++)
        if (jdv_wire_push_v2_item(stream, &items[i], credits[i])) goto done;
    /* D/G and every observed filesystem row have now passed checked delivery.
     * The only operation below that publishes the requested name is linkat. */
    (void)df_baseline_write(state, destination, filesystems, filesystem_count, &receipt);
    items[count] = df_baseline_data_item(&rows[count], &receipt);
    credits[count] = 0;
    jdv_writer_t measure; jdv_writer_init(&measure, query_data_measure, &credits[count]);
    if (jdv_encode(&items[count], &measure) || jdv_wire_push_v2_item(stream, &items[count], credits[count])) goto done;
    cancelled = receipt.cancelled ? receipt.cancelled : jash_query_cancel_poll(state);
    outcome.producer_status = cancelled ? cancelled : receipt.error ? 1 : 0;
    outcome.errors_seen = receipt.error && !cancelled ? 1 : 0;
    outcome.state = cancelled ? JDV_STREAM_CANCELLED : receipt.error ? JDV_STREAM_FAILED :
        page_partial ? JDV_STREAM_INCOMPLETE : JDV_STREAM_COMPLETE;
    outcome.completeness = outcome.state == JDV_STREAM_COMPLETE ? JDV_COMPLETENESS_COMPLETE : JDV_COMPLETENESS_PARTIAL;
    outcome.truncated = !cancelled && page_partial;
    const char* reason = cancelled ? "request_cancelled" : receipt.error ? receipt.error : page_partial ? "page_limit" : NULL;
    if (reason) outcome.reason = (jdv_slice_t){(const uint8_t*)reason, strlen(reason)};
    if (jdv_wire_push_v2_end(stream, &outcome, &final)) goto done;
    result = outcome.producer_status;
done:
    jdv_wire_push_v2_close(stream);
    free(credits); free(workspace);
    if (sigaction(SIGPIPE, &previous, NULL)) return 125;
    return result;
}

#endif
