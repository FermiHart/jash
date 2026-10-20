// SPDX-License-Identifier: LicenseRef-SPL-1.0
#include "graph_native_engine.inc"

typedef struct {
    const char* operation;
    const char* arguments[2]; int argument_count;
    unsigned top;
    uint64_t budget;
} gn_options_t;

static int gn_options(gn_options_t* options, int argc, char** argv) {
    memset(options, 0, sizeof *options); options->top = 10;
    options->operation = argc ? argv[0] : "help";
    int positional = 0, top_seen = 0, budget_seen = 0;
    for (int i = 1; i < argc; i++) {
        if (!positional && !strcmp(argv[i], "--")) { positional = 1; continue; }
        if (!positional && (!strcmp(argv[i], "--top") || !strcmp(argv[i], "--budget"))) {
            int top = !strcmp(argv[i], "--top"); uint64_t number;
            if (top ? top_seen++ : budget_seen++) return -1;
            if (++i == argc || memory_unsigned(argv[i], top ? LG_CAP_NODES : SEMANTIC_OUTPUT_BYTES, &number) || !number) return -1;
            if (top) options->top = (unsigned)number; else options->budget = number;
        } else {
            if ((!positional && argv[i][0] == '-') || options->argument_count == 2) return -1;
            options->arguments[options->argument_count++] = argv[i];
        }
    }
    const char* operation = options->operation;
    if (!strcmp(operation, "--help") || !strcmp(operation, "-h")) operation = options->operation = "help";
    int expected = !strcmp(operation, "explain") ? 1 : !strcmp(operation, "path") ? 2 :
        !strcmp(operation, "build") ? -1 : 0;
    if (expected < 0 ? options->argument_count > 1 : options->argument_count != expected) return -1;
    if (strcmp(operation, "help") && strcmp(operation, "build") && strcmp(operation, "explain") &&
        strcmp(operation, "path") && strcmp(operation, "god-nodes") && strcmp(operation, "communities") &&
        strcmp(operation, "hot") && strcmp(operation, "stats") && strcmp(operation, "push") && strcmp(operation, "sync")) return -1;
    if (top_seen && strcmp(operation, "explain") && strcmp(operation, "god-nodes") &&
        strcmp(operation, "communities") && strcmp(operation, "hot")) return -1;
    return 0;
}

static const char* gn_node_name(int index) {
    return N[index].type == 'M' ? F[N[index].fidx].path : N[index].name;
}

static int gn_lookup(const char* query) {
    for (int module = 0; module < 2; module++) for (int match = 0; match < 3; match++)
        for (int i = 0; i < nN; i++) {
            if ((N[i].type == 'M') != module) continue;
            const char* name = gn_node_name(i);
            if ((!match && !strcmp(name, query)) || (match == 1 && !strncmp(name, query, strlen(query))) ||
                (match == 2 && strstr(name, query))) return i;
        }
    return -1;
}

static jdv_value_t gn_node(semantic_value_t* values, int index) {
    if (index < 0) return jdv_value_nothing();
    char type[2] = {N[index].type, 0};
    jdv_record_input_t fields[] = {
        semantic_field("id", jdv_value_u64((uint64_t)index)),
        semantic_field("name", semantic_text(values, gn_node_name(index))),
        semantic_field("type", semantic_symbol(values, type)),
        semantic_field("file", semantic_path(values, F[N[index].fidx].path)),
        semantic_field("line", jdv_value_u64((uint64_t)N[index].line)),
        semantic_field("community", jdv_value_u64((uint64_t)N[index].community)),
        semantic_field("degree", jdv_value_u64((uint64_t)deg[index]))
    };
    return semantic_record(values, fields, 7);
}

static jdv_value_t gn_edge(semantic_value_t* values, int index, int from, int to) {
    jdv_record_input_t fields[] = {
        semantic_field("from", gn_node(values, from)), semantic_field("to", gn_node(values, to)),
        semantic_field("relation", semantic_symbol(values, lg_rel_out(E[index].rel))),
        semantic_field("direction", semantic_symbol(values, E[index].src == (unsigned)from ? "forward" : "reverse")),
        semantic_field("confidence", semantic_symbol(values, lg_conf_name(E[index].conf)))
    };
    return semantic_record(values, fields, 5);
}

static jdv_value_t gn_counts(semantic_value_t* values) {
    static const char kind_names[] = "FSETM", relation_names[] = "CKI";
    uint64_t kinds[5] = {0}, relations[3] = {0}, confidence[2] = {0}, communities = 0;
    for (int i = 0; i < nN; i++) {
        const char* kind = strchr(kind_names, N[i].type); if (kind) kinds[kind - kind_names]++;
        if ((uint64_t)N[i].community + 1 > communities) communities = (uint64_t)N[i].community + 1;
    }
    for (int i = 0; i < nE; i++) { const char* relation = strchr(relation_names, E[i].rel); if (relation) relations[relation - relation_names]++; confidence[E[i].conf != 'E']++; }
    jdv_record_input_t fields[] = {
        semantic_field("files", jdv_value_u64((uint64_t)nF)), semantic_field("nodes", jdv_value_u64((uint64_t)nN)),
        semantic_field("edges", jdv_value_u64((uint64_t)nE)), semantic_field("communities", jdv_value_u64(communities)),
        semantic_field("functions", jdv_value_u64(kinds[0])), semantic_field("structs", jdv_value_u64(kinds[1])),
        semantic_field("enums", jdv_value_u64(kinds[2])), semantic_field("typedefs", jdv_value_u64(kinds[3])),
        semantic_field("modules", jdv_value_u64(kinds[4])), semantic_field("calls", jdv_value_u64(relations[0])),
        semantic_field("contains", jdv_value_u64(relations[1])), semantic_field("includes", jdv_value_u64(relations[2])),
        semantic_field("extracted", jdv_value_u64(confidence[0])), semantic_field("inferred", jdv_value_u64(confidence[1]))
    };
    return semantic_record(values, fields, 14);
}

static jdv_value_t gn_receipt(semantic_builtin_t* call, const gn_options_t* options, jdv_value_t result,
                              int present, int attempted, memory_write_t publication, const char* error) {
    semantic_value_t* values = &call->values;
    jdv_record_input_t effects[] = {
        semantic_field("attempted", jdv_value_bool(attempted)),
        semantic_field("applied", jdv_value_bool(publication.applied)),
        semantic_field("durable", attempted ? jdv_value_bool(publication.durable) : jdv_value_nothing()),
        semantic_field("metadata_may_have_changed", jdv_value_bool(call->admitted))
    };
    jdv_record_input_t fields[] = {
        semantic_field("schema", semantic_symbol(values, "jash.graph.v1")),
        semantic_field("operation", semantic_symbol(values, options->operation)),
        semantic_field("repository_key", semantic_text(values, jash_repo_key())),
        semantic_field("source_present", jdv_value_bool(present)),
        semantic_field("source_basis", semantic_symbol(values, "persisted_graph_with_recorded_mtime_and_size")),
        semantic_field("result", result),
        semantic_field("publication", semantic_record(values, effects, 4)),
        semantic_field("complete", jdv_value_bool(!error)),
        semantic_field("error", semantic_symbol(values, error))
    };
    return semantic_record(values, fields, 9);
}

static int gn_preflight(semantic_builtin_t* call, const gn_options_t* options, const jdv_value_t* value) {
    int result = semantic_preflight(call, value);
    if (result || !options->budget) return result;
    value_output_measure_t measured = {0, SEMANTIC_OUTPUT_BYTES};
    if (call->mode == 1) result = value_output_json(semantic_measure_output, &measured, "graph", value,
        &call->values.limits, SEMANTIC_OUTPUT_BYTES, 0);
    else {
        jdv_schema_t schema = {.kind = JDV_SCHEMA_RECORD, .as.record = {.open = 1}};
        result = value_output_data(semantic_measure_output, &measured, value, 1, NULL, &schema,
            &call->values.limits, SEMANTIC_OUTPUT_BYTES, 0, NULL);
    }
    return result ? result : measured.bytes > options->budget ? 75 : 0;
}

static int gn_deliver(semantic_builtin_t* call, const gn_options_t* options, jdv_value_t value, int status, int healthy) {
    int preflight = gn_preflight(call, options, &value);
    if (preflight) return semantic_error(call, preflight == 75 ? "output_budget" : "semantic_value_limit", preflight);
    if (!healthy) return semantic_finish(call, value, status);
    jdv_stream_end_t outcome; (void)jdv_stream_end_init(&outcome);
    outcome.state = JDV_STREAM_COMPLETE; outcome.completeness = JDV_COMPLETENESS_COMPLETE;
    outcome.producer_status = status;
    return semantic_finish_outcome(call, value, &outcome);
}

static jdv_value_t gn_build_result(semantic_value_t* values, const char* root, const lg_build_observation_t* observed) {
    jdv_record_input_t fields[] = {
        semantic_field("root", semantic_path(values, root)), semantic_field("counts", gn_counts(values)),
        semantic_field("reused", jdv_value_u64((uint64_t)observed->reused)),
        semantic_field("reparsed", jdv_value_u64((uint64_t)observed->reparsed)),
        semantic_field("gone", jdv_value_u64((uint64_t)observed->gone)),
        semantic_field("skipped", jdv_value_u64((uint64_t)observed->skipped)),
        semantic_field("visited", jdv_value_u64(gn_control->visited)),
        semantic_field("recognized_source_bytes", jdv_value_filesize(gn_control->source_bytes)),
        semantic_field("excluded", jdv_value_u64(gn_control->excluded)),
        semantic_field("reuse_basis", semantic_symbol(values, "recorded_mtime_seconds_and_size")),
        semantic_field("truncated", jdv_value_bool(lg_truncated))
    };
    return semantic_record(values, fields, 11);
}

static int gn_publish_prepared(semantic_builtin_t* call, const gn_options_t* options, jdv_value_t result,
                                int mesh, int present, const char* expected_hash, int change) {
    memory_write_t publication = {0}; int attempted = 0;
    /* Error string and effect fields reserve the longest actual receipt before
     * any directory/lock creation or publication. */
    jdv_value_t candidate = gn_receipt(call, options, result, present, 1,
        (memory_write_t){1, 0, EIO}, "graph_preparation_or_publication_failed");
    int status = gn_preflight(call, options, &candidate);
    if (status) return semantic_error(call, status == 75 ? "output_budget" : "receipt_limit", status);
    if (semantic_admit(call)) return 125;
    memory_store_t store; const char* error = NULL;
    if (gn_store_open(&store, mesh, 1)) error = "graph_lock_failed";
    char name[128], lock_name[128]; int actual_present = 0;
    if (!error && (gn_names(name, lock_name, mesh) || gn_read(&store, name, 1, &actual_present, NULL))) error = "graph_read_failed";
    if (!error && (present != actual_present || strcmp(expected_hash, gn_control->last_hash))) error = "graph_changed";
    if (!error && gn_poll()) error = "cancelled";
    if (!error && change) { attempted = 1; publication = gn_publish(&store, name); if (publication.error) error = "graph_publication_failed"; }
    if (gn_store_close(&store) && !error) error = "graph_close_failed";
    status = call->cancellation_status ? call->cancellation_status : error ? 1 : 0;
    candidate = gn_receipt(call, options, result, present, attempted, publication, error);
    return semantic_finish(call, candidate, status);
}

static int gn_build(semantic_builtin_t* call, const gn_options_t* options) {
    const char* operand = options->argument_count ? options->arguments[0] : ".";
    char* root = realpath(operand, NULL);
    if (!root) return semantic_error(call, "root_unavailable", 1);
    gn_control->source_root = open(root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (gn_control->source_root < 0) { free(root); return semantic_error(call, "root_unavailable", 1); }
    memory_store_t store; char name[128], lock_name[128], fingerprint[65] = ""; int present = 0;
    int error = gn_store_open(&store, 0, 0) || gn_names(name, lock_name, 0);
    onN = onE = onF = 0;
    if (!error && gn_read(&store, name, 1, &present, NULL)) error = 1;
    if (!error) strcpy(fingerprint, gn_control->last_hash);
    if (gn_store_close(&store)) error = 1;
    lg_build_observation_t observed = {0}; lg_truncated = 0;
    memset(lg_seen, 0, LG_CAP_NODES * sizeof *lg_seen); seen_stamp = 0;
    if (!error && lg_build_inventory(root, gn_walk, gn_extract, &observed)) error = 1;
    if (close(gn_control->source_root)) error = 1;
    gn_control->source_root = -1;
    if (lg_truncated || observed.skipped || gn_control->error) error = 1;
    jdv_value_t result = gn_build_result(&call->values, root, &observed); free(root);
    if (error) {
        const char* reason = call->cancellation_status ? "cancelled" : lg_truncated ? "graph_limit" : "graph_observation_failed";
        jdv_value_t receipt = gn_receipt(call, options, result, present, 0, (memory_write_t){0}, reason);
        return gn_deliver(call, options, receipt, call->cancellation_status ? call->cancellation_status : 1, 0);
    }
    return gn_publish_prepared(call, options, result, 0, present, fingerprint,
                              observed.reparsed || observed.gone || !present);
}

static int gn_merge(semantic_builtin_t* call, const gn_options_t* options) {
    int push = !strcmp(options->operation, "push"), local_present = 0, fleet_present = 0;
    memory_store_t local, fleet; char name[128], lock_name[128], local_hash[65] = "", fleet_hash[65] = "";
    local = fleet = (memory_store_t){-1, -1}; const char* error = NULL;
    lg_truncated = 0; onN = onE = onF = nN = nE = nF = 0;
    if (gn_names(name, lock_name, 0) || gn_store_open(&local, 0, 0) || gn_store_open(&fleet, 1, 0)) error = "graph_store_unavailable";
    if (!error && fleet.directory < 0) error = "mesh_inactive";
    if (!error && gn_read(&local, name, 0, &local_present, NULL)) error = "local_graph_invalid";
    if (!error) strcpy(local_hash, gn_control->last_hash);
    if (!error && gn_read(&fleet, name, 1, &fleet_present, NULL)) error = "fleet_graph_invalid";
    if (!error) strcpy(fleet_hash, gn_control->last_hash);
    if (gn_store_close(&local) && !error) error = "graph_close_failed";
    if (gn_store_close(&fleet) && !error) error = "graph_close_failed";
    if (!error && (push ? !local_present : !fleet_present)) error = "graph_unavailable";
    if (error) return semantic_error(call, error, call->cancellation_status ? call->cancellation_status : 1);
    lg_merge_fleet(); memset(lg_seen, 0, LG_CAP_NODES * sizeof *lg_seen); seen_stamp = 0;
    (void)lg_communities();
    if (lg_truncated) return semantic_error(call, "graph_limit", 1);
    jdv_record_input_t fields[] = {
        semantic_field("counts", gn_counts(&call->values)),
        semantic_field("nodes_added_from_fleet", jdv_value_u64((uint64_t)lg_merge_nodes_added)),
        semantic_field("edges_added_from_fleet", jdv_value_u64((uint64_t)lg_merge_edges_added)),
        semantic_field("confidence_upgrades", jdv_value_u64((uint64_t)lg_merge_upgraded)),
        semantic_field("destination", semantic_symbol(&call->values, push ? "fleet" : "local"))
    };
    jdv_value_t result = semantic_record(&call->values, fields, 5);
    int change = push || !local_present || lg_merge_nodes_added || lg_merge_edges_added || lg_merge_upgraded;
    return gn_publish_prepared(call, options, result, push, push ? fleet_present : local_present,
                               push ? fleet_hash : local_hash, change);
}

static int gn_rank_degree(const void* a, const void* b) {
    int left = *(const int*)a, right = *(const int*)b;
    return deg[left] == deg[right] ? (left > right) - (left < right) :
        (deg[left] < deg[right]) - (deg[left] > deg[right]);
}

typedef struct { int index; uint64_t score; } gn_rank_t;
static int gn_rank_score(const void* a, const void* b) {
    const gn_rank_t *left = a, *right = b;
    return left->score == right->score ? (left->index > right->index) - (left->index < right->index) :
        (left->score < right->score) - (left->score > right->score);
}

static jdv_value_t gn_explain(semantic_builtin_t* call, const gn_options_t* options, int* status) {
    semantic_value_t* values = &call->values;
    int node = gn_lookup(options->arguments[0]); *status = node < 0 ? 1 : 0;
    size_t count = node < 0 ? 0 : (unsigned)deg[node] < options->top ? (size_t)deg[node] : options->top;
    jdv_value_t* edges = calloc(count ? count : 1, sizeof *edges);
    if (!edges) { values->error = 1; return jdv_value_nothing(); }
    for (size_t i = 0; i < count; i++) {
        int at = adj_off[node] + (int)i;
        edges[i] = gn_edge(values, adj_edge[at], node, adj_dst[at]);
    }
    jdv_record_input_t fields[] = {
        semantic_field("query", semantic_text(values, options->arguments[0])),
        semantic_field("lookup_basis", semantic_symbol(values, "exact_then_prefix_then_substring_nonmodules_first")),
        semantic_field("found", jdv_value_bool(node >= 0)), semantic_field("node", gn_node(values, node)),
        semantic_field("edges", semantic_list(values, edges, count)),
        semantic_field("connections", jdv_value_u64(node < 0 ? 0 : (uint64_t)deg[node])),
        semantic_field("returned", jdv_value_u64(count)), semantic_field("has_more", jdv_value_bool(node >= 0 && (size_t)deg[node] > count))
    };
    jdv_value_t result = semantic_record(values, fields, 8); free(edges); return result;
}

static jdv_value_t gn_path(semantic_builtin_t* call, const gn_options_t* options, int* status) {
    semantic_value_t* values = &call->values;
    int first = gn_lookup(options->arguments[0]), last = gn_lookup(options->arguments[1]), found = 0, searched = 0;
    memset(visited, 0, (size_t)nN);
    if (first >= 0 && last >= 0) {
        int head = 0, tail = 0; queue[tail++] = first; visited[first] = 1; prev_node[first] = -1; prev_edge[first] = -1;
        while (head < tail) {
            if (gn_poll()) break;
            int node = queue[head++]; searched++;
            if (node == last) { found = 1; break; }
            for (int at = adj_off[node]; at < adj_off[node + 1]; at++) {
                int next = adj_dst[at]; if (visited[next]) continue;
                visited[next] = 1; prev_node[next] = node; prev_edge[next] = adj_edge[at]; queue[tail++] = next;
            }
        }
    }
    int count = 0;
    if (found) for (int node = last; prev_node[node] >= 0; node = prev_node[node]) count++;
    jdv_value_t* hops = calloc(count ? (size_t)count : 1, sizeof *hops);
    if (!hops) { values->error = 1; return jdv_value_nothing(); }
    if (found) {
        int index = count;
        for (int node = last; prev_node[node] >= 0; node = prev_node[node])
            hops[--index] = gn_edge(values, prev_edge[node], prev_node[node], node);
    }
    *status = call->cancellation_status ? call->cancellation_status : found ? 0 : 1;
    jdv_record_input_t fields[] = {
        semantic_field("from_query", semantic_text(values, options->arguments[0])),
        semantic_field("to_query", semantic_text(values, options->arguments[1])),
        semantic_field("from", gn_node(values, first)), semantic_field("to", gn_node(values, last)),
        semantic_field("search_basis", semantic_symbol(values, "breadth_first_undirected_all_relations")),
        semantic_field("found", jdv_value_bool(found)), semantic_field("searched", jdv_value_u64((uint64_t)searched)),
        semantic_field("hops", semantic_list(values, hops, (size_t)count))
    };
    jdv_value_t result = semantic_record(values, fields, 8); free(hops); return result;
}

static jdv_value_t gn_god_nodes(semantic_builtin_t* call, const gn_options_t* options) {
    semantic_value_t* values = &call->values;
    int* indices = calloc(nN ? (size_t)nN : 1, sizeof *indices);
    size_t count = (unsigned)nN < options->top ? (size_t)nN : options->top;
    jdv_value_t* nodes = calloc(count ? count : 1, sizeof *nodes);
    if (!indices || !nodes) { free(indices); free(nodes); values->error = 1; return jdv_value_nothing(); }
    for (int i = 0; i < nN; i++) indices[i] = i;
    qsort(indices, (size_t)nN, sizeof *indices, gn_rank_degree);
    for (size_t i = 0; i < count; i++) nodes[i] = gn_node(values, indices[i]);
    jdv_record_input_t fields[] = {
        semantic_field("total", jdv_value_u64((uint64_t)nN)), semantic_field("returned", jdv_value_u64(count)),
        semantic_field("has_more", jdv_value_bool(count < (size_t)nN)),
        semantic_field("rank_basis", semantic_symbol(values, "total_undirected_degree_then_store_index")),
        semantic_field("nodes", semantic_list(values, nodes, count))
    };
    jdv_value_t result = semantic_record(values, fields, 5); free(indices); free(nodes); return result;
}

static jdv_value_t gn_community_result(semantic_builtin_t* call, const gn_options_t* options) {
    semantic_value_t* values = &call->values; int communities = 0;
    for (int i = 0; i < nN; i++) if (N[i].community + 1 > communities) communities = N[i].community + 1;
    gn_rank_t* ranked = calloc(communities ? (size_t)communities : 1, sizeof *ranked);
    int* top = malloc((communities ? (size_t)communities : 1) * 3 * sizeof *top);
    size_t count = (unsigned)communities < options->top ? (size_t)communities : options->top;
    jdv_value_t* rows = calloc(count ? count : 1, sizeof *rows);
    if (!ranked || !top || !rows) { free(ranked); free(top); free(rows); values->error = 1; return jdv_value_nothing(); }
    for (int i = 0; i < communities; i++) { ranked[i].index = i; top[i * 3] = top[i * 3 + 1] = top[i * 3 + 2] = -1; }
    for (int i = 0; i < nN; i++) {
        int community = N[i].community; ranked[community].score++;
        for (int slot = 0; slot < 3; slot++) {
            int* nodes = &top[community * 3];
            if (nodes[slot] < 0 || deg[i] > deg[nodes[slot]]) {
                for (int shift = 2; shift > slot; shift--) nodes[shift] = nodes[shift - 1];
                nodes[slot] = i; break;
            }
        }
    }
    qsort(ranked, (size_t)communities, sizeof *ranked, gn_rank_score);
    for (size_t i = 0; i < count; i++) {
        int index = ranked[i].index; jdv_value_t nodes[3]; size_t keep = 0;
        for (int j = 0; j < 3 && top[index * 3 + j] >= 0; j++) nodes[keep++] = gn_node(values, top[index * 3 + j]);
        jdv_record_input_t fields[] = {
            semantic_field("id", jdv_value_u64((uint64_t)index)), semantic_field("size", jdv_value_u64(ranked[i].score)),
            semantic_field("top_nodes", semantic_list(values, nodes, keep))
        };
        rows[i] = semantic_record(values, fields, 3);
    }
    jdv_record_input_t fields[] = {
        semantic_field("total", jdv_value_u64((uint64_t)communities)), semantic_field("returned", jdv_value_u64(count)),
        semantic_field("has_more", jdv_value_bool(count < (size_t)communities)),
        semantic_field("communities", semantic_list(values, rows, count))
    };
    jdv_value_t result = semantic_record(values, fields, 4); free(ranked); free(top); free(rows); return result;
}

static const jdv_value_t* gn_field(const jdv_value_t* record, const char* key) {
    if (record->kind != JDV_RECORD) return NULL;
    for (size_t i = 0; i < record->as.record.len; i++) {
        const jdv_record_field_t* field = &record->as.record.fields[i];
        if (field->key.len == strlen(key) && !memcmp(field->key.data, key, field->key.len)) return &field->value;
    }
    return NULL;
}

static int gn_contains(const jdv_value_t* text, const char* needle) {
    if (!text || (text->kind != JDV_STRING && text->kind != JDV_BYTES)) return 0;
    size_t length = strlen(needle);
    if (!length) return 0;
    for (size_t i = 0; i < text->as.slice.len && length <= text->as.slice.len - i; i++)
        if (!memcmp(text->as.slice.data + i, needle, length)) return 1;
    return 0;
}

static jdv_value_t gn_hot(semantic_builtin_t* call, const gn_options_t* options, int* status) {
    semantic_value_t* values = &call->values;
    memory_chain_t chain; *status = memory_chain_load(&chain, 0, 0);
    jdv_value_t source = jdv_value_nothing(), gaps = jdv_value_nothing();
    if (!*status) *status = jash_gap_observe_rows(values, &source, &gaps);
    uint64_t* touches = calloc(nF ? (size_t)nF : 1, sizeof *touches);
    uint64_t* failures = calloc(nF ? (size_t)nF : 1, sizeof *failures);
    gn_rank_t* ranked = calloc(nF ? (size_t)nF : 1, sizeof *ranked);
    if (!touches || !failures || !ranked) *status = 125;
    for (int i = 0; i < LG_HFILE; i++) bhash[i] = -1;
    for (int i = 0; i < nF; i++) {
        unsigned bucket = lg_fnv(lg_basename(F[i].path)) & (LG_HFILE - 1);
        bnext[i] = bhash[bucket]; bhash[bucket] = i; fstamp[i] = -1;
    }
    for (size_t line = 0; !*status && line < chain.count; line++) {
        if (gn_poll()) { *status = call->cancellation_status; break; }
        char* copy = strdup(chain.entries[line].command); if (!copy) { *status = 125; break; }
        char* save = NULL;
        for (char* token = strtok_r(copy, " /\t\n\r\"'", &save); token; token = strtok_r(NULL, " /\t\n\r\"'", &save)) {
            unsigned bucket = lg_fnv(token) & (LG_HFILE - 1);
            for (int i = bhash[bucket]; i >= 0; i = bnext[i]) {
                if (fstamp[i] == (int)line || strcmp(lg_basename(F[i].path), token)) continue;
                fstamp[i] = (int)line; touches[i]++; if (chain.entries[line].status) failures[i]++;
            }
        }
        free(copy);
    }
    uint64_t touched = 0, failed = 0;
    if (!*status) for (int i = 0; i < nF; i++) {
        ranked[i] = (gn_rank_t){i, touches[i] + 3 * failures[i]}; touched += touches[i] != 0; failed += failures[i];
    }
    if (!*status) qsort(ranked, (size_t)nF, sizeof *ranked, gn_rank_score);
    size_t count = *status ? 0 : touched < options->top ? (size_t)touched : options->top;
    jdv_value_t* rows = calloc(count ? count : 1, sizeof *rows); if (!rows) { *status = 125; count = 0; }
    for (size_t i = 0; i < count; i++) {
        int file = ranked[i].index, top_node = -1, match = 0;
        for (int j = 0; j < nN; j++) if (N[j].fidx == file && N[j].type != 'M' && (top_node < 0 || deg[j] > deg[top_node])) top_node = j;
        for (size_t j = 0; gaps.kind == JDV_LIST && j < gaps.as.list.len; j++) {
            const jdv_value_t* expected = gn_field(&gaps.as.list.items[j], "expected");
            const jdv_value_t* tried = gn_field(&gaps.as.list.items[j], "tried");
            if (gn_contains(expected, lg_basename(F[file].path)) || gn_contains(tried, lg_basename(F[file].path)) ||
                (top_node >= 0 && (gn_contains(expected, gn_node_name(top_node)) || gn_contains(tried, gn_node_name(top_node))))) { match = 1; break; }
        }
        jdv_record_input_t fields[] = {
            semantic_field("file", semantic_path(values, F[file].path)), semantic_field("pain", jdv_value_u64(ranked[i].score)),
            semantic_field("touches", jdv_value_u64(touches[file])), semantic_field("failures", jdv_value_u64(failures[file])),
            semantic_field("top_node", gn_node(values, top_node)), semantic_field("gap_match", jdv_value_bool(match))
        };
        rows[i] = semantic_record(values, fields, 6);
    }
    jdv_record_input_t fields[] = {
        semantic_field("chain_path", semantic_path(values, chain.path)), semantic_field("chain_present", jdv_value_bool(chain.present)),
        semantic_field("chain_records", jdv_value_u64(chain.count)), semantic_field("cryptographically_verified", jdv_value_bool(0)),
        semantic_field("gap_source", source), semantic_field("match_basis", semantic_symbol(values, "recorded_command_path_components_and_basenames")),
        semantic_field("gap_match_basis", semantic_symbol(values, "tried_or_expected_substring_of_basename_or_top_node")),
        semantic_field("pain_formula", semantic_symbol(values, "touches_plus_three_times_failures")),
        semantic_field("files_touched", jdv_value_u64(touched)), semantic_field("file_failure_matches", jdv_value_u64(failed)),
        semantic_field("returned", jdv_value_u64(count)), semantic_field("has_more", jdv_value_bool(touched > count)),
        semantic_field("files", semantic_list(values, rows, count))
    };
    jdv_value_t result = semantic_record(values, fields, 13);
    free(touches); free(failures); free(ranked); free(rows); memory_chain_free(&chain); return result;
}

static int gn_run(semantic_builtin_t* call, jash_state_t* state, int argc, char** argv) {
    gn_options_t options;
    if (gn_options(&options, argc, argv)) return semantic_error(call, "invalid_graph_arguments", 2);
    gn_control_t control = {.state = state, .call = call, .source_root = -1}; gn_control = &control;
    call->values.limits.max_record_fields = 65536;
    int status = 0;
    if (!strcmp(options.operation, "help")) {
        jdv_record_input_t fields[] = {
            semantic_field("usage", semantic_text(&call->values, "graph --json|--data|--data-v2 build [DIR] | explain NAME | path A B | god-nodes | communities | hot | stats | push | sync; --top N; --budget BYTES; --")),
            semantic_field("node_limit", jdv_value_u64(LG_CAP_NODES)), semantic_field("edge_limit", jdv_value_u64(LG_CAP_EDGES)),
            semantic_field("file_limit", jdv_value_u64(LG_CAP_FILES)), semantic_field("source_file_bytes", jdv_value_filesize(GN_FILE_BYTES)),
            semantic_field("source_total_bytes", jdv_value_filesize(GN_SOURCE_BYTES)), semantic_field("store_bytes", jdv_value_filesize(GN_STORE_BYTES)),
            semantic_field("relative_path_bytes", jdv_value_u64(383)), semantic_field("function_name_bytes", jdv_value_u64(119)),
            semantic_field("source_scope", semantic_text(&call->values, "C and headers; skip dot directories, build* directories, node_modules, scratchpad, Quarentena-codes and symlinks; tab/CR/LF paths rejected before publication"))
        };
        jdv_value_t result = semantic_record(&call->values, fields, 10);
        status = gn_deliver(call, &options, gn_receipt(call, &options, result, 0, 0, (memory_write_t){0}, NULL), 0, 1);
        gn_control = NULL; return status;
    }
    if (!lg_alloc()) { gn_control = NULL; return semantic_error(call, "allocation_failed", 125); }
    if (!strcmp(options.operation, "build")) status = gn_build(call, &options);
    else if (!strcmp(options.operation, "push") || !strcmp(options.operation, "sync")) status = gn_merge(call, &options);
    else {
        memory_store_t store; int present = 0; char name[128], lock_name[128]; lg_truncated = 0;
        int error = gn_store_open(&store, 0, 0) || gn_names(name, lock_name, 0);
        if (!error && gn_read(&store, name, 0, &present, NULL)) error = 1;
        if (gn_store_close(&store)) error = 1;
        if (error || !present) status = semantic_error(call, error ? "graph_read_failed" : "graph_unavailable", call->cancellation_status ? call->cancellation_status : 1);
        else {
            lg_build_adj(); jdv_value_t result;
            if (!strcmp(options.operation, "stats")) result = gn_counts(&call->values);
            else if (!strcmp(options.operation, "explain")) result = gn_explain(call, &options, &status);
            else if (!strcmp(options.operation, "path")) result = gn_path(call, &options, &status);
            else if (!strcmp(options.operation, "god-nodes")) result = gn_god_nodes(call, &options);
            else if (!strcmp(options.operation, "communities")) result = gn_community_result(call, &options);
            else result = gn_hot(call, &options, &status);
            const char* failure = call->cancellation_status ? "cancelled" : lg_truncated ? "graph_limit" :
                status && !strcmp(options.operation, "hot") ? "history_source_unavailable" : NULL;
            if (lg_truncated && !status) status = 1;
            result = gn_receipt(call, &options, result, present, 0, (memory_write_t){0}, failure);
            status = gn_deliver(call, &options, result, status, !failure);
        }
    }
    gn_control = NULL; return status;
}
