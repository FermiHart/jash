// SPDX-License-Identifier: LicenseRef-SPL-1.0
/* Delta observations and publication receipts share the checked gold store.
 * No human output is captured and a baseline is never inferred from read error. */
static jdv_value_t lsxd_source(semantic_value_t* values, const jash_gold_delta_observation_t* observed,
                               int collection_error, const lsx_query_t* query) {
    jdv_record_input_t fields[] = {
        semantic_field("path", semantic_path(values, observed->path[0] ? observed->path : NULL)),
        semantic_field("present", jdv_value_bool(observed->present)), semantic_field("read_complete", jdv_value_bool(observed->complete)),
        semantic_field("entries", observed->complete ? jdv_value_u64(observed->count) : jdv_value_nothing()),
        semantic_field("bytes", jdv_value_filesize(observed->bytes)),
        semantic_field("errno", observed->error_number ? jdv_value_i64(observed->error_number) : jdv_value_nothing()),
        semantic_field("collection_complete", jdv_value_bool(!collection_error && !query->truncated && !query->cancelled && query->status != 125)),
        semantic_field("collection_errno", collection_error ? jdv_value_i64(collection_error) : jdv_value_nothing()),
        semantic_field("collection_reason", semantic_symbol(values, query->reason))
    };
    return semantic_record(values, fields, 9);
}
static jdv_value_t lsxd_header(semantic_value_t* values, const char* path, const char* key,
                               const jash_gold_delta_observation_t* observed, int collection_error,
                               const lsx_query_t* query) {
    jdv_record_input_t provenance[] = {
        semantic_field("fingerprint", semantic_symbol(values, "mtime_ns_size_type")),
        semantic_field("content_hashed", jdv_value_bool(0)), semantic_field("key_algorithm", semantic_symbol(values, "legacy_fnv1a32_session_absolute_path_hidden_suffix")),
        semantic_field("snapshot_format", semantic_symbol(values, "JGOLD2_binary")),
        semantic_field("legacy_read", semantic_symbol(values, "JGOLD1_strict")), semantic_field("compare_before_publish", jdv_value_bool(1))
    };
    jdv_record_input_t fields[] = {
        semantic_field("schema", semantic_symbol(values, "jash.ls.delta.row.v1")), semantic_field("row_kind", semantic_symbol(values, "header")),
        semantic_field("directory", semantic_path(values, path)), semantic_field("session", semantic_text(values, jash_gold_session())),
        semantic_field("key", semantic_text(values, key)), semantic_field("include_hidden", jdv_value_bool(query->options.all)), semantic_field("baseline", jdv_value_bool(observed->complete && !observed->present)),
        semantic_field("source", lsxd_source(values, observed, collection_error, query)),
        semantic_field("provenance", semantic_record(values, provenance, 6))
    };
    return semantic_record(values, fields, 9);
}
static jdv_value_t lsxd_change(semantic_value_t* values, const char* change,
                               const jash_gold_dent_t* entry, const jash_gold_dent_t* previous) {
    char type[2] = {entry->tag, 0};
    jdv_record_input_t fields[] = {
        semantic_field("schema", semantic_symbol(values, "jash.ls.delta.row.v1")), semantic_field("row_kind", semantic_symbol(values, "change")),
        semantic_field("change", semantic_symbol(values, change)), semantic_field("name", semantic_text(values, entry->name)),
        semantic_field("type", semantic_symbol(values, type)), semantic_field("size", jdv_value_filesize((uint64_t)entry->size)),
        semantic_field("modified", jdv_value_timestamp((int64_t)entry->fp)),
        semantic_field("size_delta", previous ? jdv_value_i64((int64_t)entry->size - (int64_t)previous->size) : jdv_value_nothing())
    };
    return semantic_record(values, fields, 8);
}
static jdv_value_t lsxd_summary(semantic_value_t* values, const jash_gold_delta_observation_t* observed,
                                const jash_gold_delta_publication_t* publication,
                                const lsx_query_t* query, size_t entries, size_t changes, int worst) {
    jdv_record_input_t receipt[] = {
        semantic_field("path", semantic_path(values, observed->path[0] ? observed->path : NULL)),
        semantic_field("applied", jdv_value_bool(worst ? 0 : publication->applied)),
        semantic_field("durable", jdv_value_bool(worst ? 0 : publication->durable)),
        semantic_field("directories_created", jdv_value_u64(worst ? UINT64_MAX : (uint64_t)publication->directories_created)),
        semantic_field("lock_created", jdv_value_bool(worst ? 0 : publication->lock_created)),
        semantic_field("bytes_written", jdv_value_filesize(worst ? UINT64_MAX : publication->bytes_written)),
        semantic_field("phase", semantic_symbol(values, worst ? "snapshot_publication_phase_reserve_for_checked_terminal" : publication->phase ? publication->phase : "not_started")),
        semantic_field("errno", worst ? jdv_value_i64(INT_MAX) : publication->error_number ? jdv_value_i64(publication->error_number) : jdv_value_nothing())
    };
    jdv_record_input_t summary[] = {
        semantic_field("baseline", jdv_value_bool(observed->complete && !observed->present)),
        semantic_field("changes", jdv_value_u64(changes)), semantic_field("entries", jdv_value_u64(entries)),
        semantic_field("since_s", observed->complete && observed->present ? jdv_value_i64(observed->age_seconds) : jdv_value_nothing()),
        semantic_field("source_complete", jdv_value_bool(observed->complete)),
        semantic_field("command_status", jdv_value_i64(worst ? INT_MAX : query->status)),
        semantic_field("cancelled", jdv_value_bool(worst ? 0 : query->cancelled != 0)),
        semantic_field("reason", semantic_symbol(values, worst ? "snapshot_publication_failure_reason_terminal_reserve" : query->reason))
    };
    jdv_record_input_t fields[] = {
        semantic_field("schema", semantic_symbol(values, "jash.ls.delta.row.v1")), semantic_field("row_kind", semantic_symbol(values, "summary")),
        semantic_field("summary", semantic_record(values, summary, 8)), semantic_field("publication", semantic_record(values, receipt, 8))
    };
    return semantic_record(values, fields, 4);
}
static int lsxd_preflight(semantic_value_t* values, const jdv_value_t* rows, size_t count,
                          const uint64_t* credits, const lsx_query_t* query,
                          const jdv_stream_config_t* config, void* workspace) {
    if (values->error) return 125;
    value_output_measure_t measure = {0, LSX_OUTPUT_BYTES - 512u};
    if (query->options.version) {
        jdv_stream_end_t outcome, final; lsx_outcome(&outcome, query);
        return jdv_emit_sequence_version((unsigned)query->options.version, rows, count, credits, config, &outcome,
            value_output_measure, &measure, workspace, LSX_FRAME_BYTES, &final) ? 125 : 0;
    }
    jash_json_writer_t json; jash_jsonw_init(&json, (jash_json_sink_t)value_output_measure, &measure);
    for (size_t i = 0; i < count; i++) {
        if (jdv_json_write(&json, &rows[i], &values->limits)) return 125;
        jash_jsonw_raw(&json, "\n");
    }
    if (jash_jsonw_finish(&json)) return 125;
    long budget = jash_budget_limit();
    return budget > 0 && (uint64_t)budget < measure.bytes + 512u ? 75 : 0;
}
static int lsxd_row(jash_structured_output_t* output, jdv_wire_push_t* push,
                     const jdv_value_t* row, uint64_t credit, const jdv_limits_t* limits) {
    if (push) return jdv_wire_push_item(push, row, credit) ? 125 : 0;
    if (jdv_json_write(&output->json, row, limits)) return 125;
    jash_jsonw_raw(&output->json, "\n");
    return jash_jsonw_finish(&output->json) ? 125 : 0;
}
static int ls_structured_delta(jash_state_t* state, jash_writer_t writer, void* context,
                               const char* path, int all, int version) {
    lsx_query_t query = {.state = state, .options = {.all = all, .version = version}};
    jash_gold_delta_observation_t observed = {0}; jash_gold_delta_publication_t publication = {0};
    char absolute[4096], key[32] = {0}; int collection_error = 0;
    if (!realpath(path, absolute)) {
        collection_error = errno; strlcpy(absolute, path, sizeof(absolute));
        lsx_failed(&query, "resolve_directory", 1, 0); query.errors++;
    } else {
        lsx_collect(&query, path, path, 0, 0, 1);
        for (size_t i = 0; i < query.count && !collection_error; i++) collection_error = query.entries[i].failure_errno ? query.entries[i].failure_errno : query.entries[i].item.error.error_number;
    }
    jash_gold_dent_t* current = calloc(LSD_MAX, sizeof(*current));
    jash_gold_dent_t* previous = calloc(LSD_MAX, sizeof(*previous));
    if (!current || !previous) { free(current); free(previous); lsx_free(&query); return 125; }
    if (!query.status && query.count > LSD_MAX) lsx_failed(&query, "delta_item_limit", 1, 1);
    for (size_t i = 0; i < query.count && !query.status; i++) {
        const jash_ls_item_t* item = &query.entries[i].item;
        int64_t timestamp;
        if (item->name.len >= sizeof(current[i].name) || !(item->metadata.available & JASH_LS_META_SIZE) ||
            item->metadata.size > INT64_MAX || lsx_timestamp_ns(item->metadata.modified, &timestamp)) {
            lsx_failed(&query, "delta_metadata_range", 1, 0); collection_error = EOVERFLOW; query.errors++; break;
        }
        memcpy(current[i].name, item->name.data, item->name.len); current[i].size = (long long)item->metadata.size;
        current[i].fp = (long long)timestamp; current[i].tag = lsd_typechar((mode_t)item->metadata.mode);
    }
    if (!query.status) {
        qsort(current, query.count, sizeof(*current), lsd_cmp);
        jash_gold_key(jash_gold_session(), absolute, key, sizeof(key));
        if (all) strcat(key, "-all");
        if (jash_gold_delta_load_checked("ls", key, previous, LSD_MAX, &observed)) {
            lsx_failed(&query, "snapshot_read", 1, 0); query.errors++;
        }
        if (!query.status) for (size_t i = 0; i < observed.count; i++) {
            if (previous[i].tag != 'f' && previous[i].tag != 'd' && previous[i].tag != 'l' && previous[i].tag != 'o') {
                observed.complete = 0; observed.error_number = EINVAL;
                lsx_failed(&query, "snapshot_type", 1, 0); query.errors++; break;
            }
        }
    }
    semantic_value_t values;
    if (semantic_value_init(&values, LSX_VALUE_BYTES, LSX_FRAME_BYTES - 512u)) { free(current); free(previous); lsx_free(&query); return 125; }
    size_t capacity = 2u * LSD_MAX + 2u;
    jdv_value_t* rows = calloc(capacity, sizeof(*rows)); uint64_t* credits = calloc(capacity, sizeof(*credits));
    void* workspace = malloc(LSX_FRAME_BYTES); int result = 125;
    if (!rows || !credits || !workspace) goto done;
    size_t count = 0, changes = 0;
    rows[count++] = lsxd_header(&values, absolute, key, &observed, collection_error, &query);
    if (!query.status) {
        size_t i = 0, j = 0;
        while (i < observed.count || j < query.count) {
            const char* change = NULL; const jash_gold_dent_t* entry = NULL; const jash_gold_dent_t* prior = NULL;
            int compare = i == observed.count ? 1 : j == query.count ? -1 : strcmp(previous[i].name, current[j].name);
            if (compare < 0) { change = "removed"; entry = &previous[i++]; }
            else if (compare > 0) { change = observed.present ? "added" : "baseline"; entry = &current[j++]; }
            else {
                if (previous[i].fp != current[j].fp || previous[i].size != current[j].size || previous[i].tag != current[j].tag) {
                    change = "modified"; entry = &current[j]; prior = &previous[i];
                }
                i++; j++;
            }
            if (change) { credits[count] = strlen(entry->name); rows[count++] = lsxd_change(&values, change, entry, prior); changes++; }
        }
    }
    size_t summary_mark = values.arena.used;
    rows[count++] = lsxd_summary(&values, &observed, &publication, &query, query.count, changes, !query.status);
    if (query.status) { result = lsx_emit_rows(writer, context, &values, rows, count, credits, &query); goto done; }
    jdv_schema_t schema = {.kind = JDV_SCHEMA_RECORD, .as.record = {.open = 1}};
    jdv_stream_config_t config; lsx_config(&config, &values, &schema);
    result = lsxd_preflight(&values, rows, count, credits, &query, &config, workspace);
    if (result) { result = jash_structured_error(writer, context, "ls", result == 75 ? "output_budget_exceeded" : "value_limit", result); goto done; }
    if (lsx_poll(&query)) {
        values.arena.used = summary_mark; rows[count-1u] = lsxd_summary(&values, &observed, &publication, &query, query.count, changes, 0);
        result = lsx_emit_rows(writer, context, &values, rows, count, credits, &query); goto done;
    }
    jash_structured_output_t output;
    if (jash_structured_begin(&output, writer, context)) { result = 125; goto done; }
    jdv_wire_push_t* push = NULL; value_output_sink_t sink = {output.json.sink, output.json.context};
    if (version && jdv_wire_push_open_version((unsigned)version, &push, &config, value_output_sink, &sink, workspace, LSX_FRAME_BYTES)) {
        result = jash_structured_finish(&output, 125); goto done;
    }
    result = 0;
    for (size_t i = 0; i + 1u < count; i++) {
        result = lsxd_row(&output, push, &rows[i], credits[i], &values.limits);
        if (result) break;
    }
    if (!result) {
        if (jash_gold_delta_store_checked("ls", key, current, query.count, &observed, lsx_poll, &query, &publication)) {
            if (!query.cancelled) lsx_failed(&query, "snapshot_publish", 1, 0);
            query.errors++;
        }
        values.arena.used = summary_mark;
        rows[count-1u] = lsxd_summary(&values, &observed, &publication, &query, query.count, changes, 0);
        result = values.error ? 125 : lsxd_row(&output, push, &rows[count-1u], 0, &values.limits);
    }
    if (!result && push) {
        jdv_stream_end_t outcome, final; lsx_outcome(&outcome, &query);
        if (jdv_wire_push_end(push, &outcome, &final)) result = 125;
    }
    jdv_wire_push_close(push);
    result = jash_structured_finish(&output, result ? result : query.status);
done:
    free(rows); free(credits); free(workspace); semantic_value_free(&values);
    free(current); free(previous); lsx_free(&query); return result;
}
