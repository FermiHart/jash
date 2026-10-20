// SPDX-License-Identifier: LicenseRef-SPL-1.0
/* Checked source models for the change engine's documented YAML/Markdown
 * subset. These parse project files, never another command's stdout. */
typedef struct {
    char uuid[128], name[CHG_MAX_NAME], schema[CHG_MAX_NAME], status[128], goal[4097];
    int64_t created;
    unsigned seen;
} cn_meta_t;
typedef struct {
    cn_meta_t meta;
    chg_schema_t schema;
    cn_file_t metadata;
    int done[CHG_MAX_ARTIFACTS];
    char path[CN_PATH];
} cn_change_t;

static int cn_scalar(const char* text, char* output, size_t capacity) {
    while (*text == ' ' || *text == '\t') text++;
    size_t size = strlen(text);
    while (size && (text[size - 1] == ' ' || text[size - 1] == '\t' || text[size - 1] == '\r')) size--;
    if (size && *text == '"') {
        unsigned char workspace[32768]; jdv_arena_t arena;
        jdv_arena_init(&arena, workspace, sizeof workspace); jdj_value_t value;
        if (jdj_parse(&arena, text, size, &JDV_DEFAULT_LIMITS, &value) || value.kind != JDJ_STRING ||
            value.scalar.len >= capacity || memchr(value.scalar.data, 0, value.scalar.len)) { errno = EINVAL; return -1; }
        memcpy(output, value.scalar.data, value.scalar.len); output[value.scalar.len] = 0; return 0;
    }
    if (size >= capacity) { errno = E2BIG; return -1; }
    memcpy(output, text, size); output[size] = 0; return 0;
}
static int cn_schema_parse(cn_store_t* store, const cn_file_t* source, chg_schema_t* schema) {
    memset(schema, 0, sizeof(*schema));
    if (!source->exists || (source->size && memchr(source->bytes, 0, source->size)))
        return cn_fail(store, "invalid_schema", EINVAL);
    char* copy = strdup(source->bytes); if (!copy) return cn_fail(store, "allocation_failed", ENOMEM);
    unsigned seen = 0, artifact_seen[CHG_MAX_ARTIFACTS] = {0}; int failed = 0;
    char* save = NULL;
    for (char* line = strtok_r(copy, "\n", &save); line && !failed; line = strtok_r(NULL, "\n", &save)) {
        size_t size = strlen(line); if (size && line[size - 1] == '\r') line[--size] = 0;
        if (!size || line[0] == '#') continue;
        if (!strncmp(line, "name:", 5)) {
            if ((seen & 1u) || cn_scalar(line + 5, schema->name, sizeof schema->name) || !cn_name(schema->name)) failed = 1;
            seen |= 1u;
        } else if (!strncmp(line, "version:", 8)) {
            char version[16]; if ((seen & 2u) || cn_scalar(line + 8, version, sizeof version) || strcmp(version, "1")) failed = 1;
            seen |= 2u;
        } else if (!strncmp(line, "description:", 12)) {
            if ((seen & 4u) || size > 4096u) failed = 1;
            seen |= 4u;
        } else if (!strcmp(line, "artifacts:")) {
            if (seen & 8u) failed = 1;
            seen |= 8u;
        } else if (!strncmp(line, "  - id:", 7) && (seen & 8u)) {
            if (schema->n_artifacts == CHG_MAX_ARTIFACTS) { failed = 1; errno = E2BIG; break; }
            chg_artifact_t* artifact = &schema->artifacts[schema->n_artifacts++];
            if (cn_scalar(line + 7, artifact->id, sizeof artifact->id) || !cn_name(artifact->id)) failed = 1;
        } else if (schema->n_artifacts && !strncmp(line, "    generates:", 14)) {
            int index = schema->n_artifacts - 1; chg_artifact_t* artifact = &schema->artifacts[index];
            if ((artifact_seen[index] & 1u) || cn_scalar(line + 14, artifact->generates, sizeof artifact->generates) ||
                (!cn_relative(artifact->generates) && strcmp(artifact->generates, "specs/**/spec.md"))) failed = 1;
            if (strpbrk(artifact->generates, "*?[" ) && (strcmp(artifact->id, "specs") || strcmp(artifact->generates, "specs/**/spec.md"))) failed = 1;
            artifact_seen[index] |= 1u;
        } else if (schema->n_artifacts && !strncmp(line, "    requires:", 13)) {
            int index = schema->n_artifacts - 1; chg_artifact_t* artifact = &schema->artifacts[index];
            char requires[2048];
            if ((artifact_seen[index] & 2u) || cn_scalar(line + 13, requires, sizeof requires)) { failed = 1; break; }
            artifact_seen[index] |= 2u;
            size_t length = strlen(requires);
            if (length < 2 || requires[0] != '[' || requires[length - 1] != ']') { failed = 1; break; }
            requires[length - 1] = 0; char* item = requires + 1;
            while (*item && !failed) {
                char* comma = strchr(item, ','); if (comma) *comma = 0;
                char dependency[CHG_MAX_NAME];
                if (cn_scalar(item, dependency, sizeof dependency) || !cn_name(dependency) || artifact->n_requires == CHG_MAX_REQUIRES) { failed = 1; break; }
                for (int j = 0; j < artifact->n_requires; j++) if (!strcmp(dependency, artifact->requires[j])) failed = 1;
                memcpy(artifact->requires[artifact->n_requires++], dependency, strlen(dependency) + 1u);
                if (!comma) break;
                item = comma + 1; if (!*item) failed = 1;
            }
        } else failed = 1;
    }
    for (int i = 0; i < schema->n_artifacts && !failed; i++) {
        if (artifact_seen[i] != 3u) failed = 1;
        for (int j = 0; j < i; j++) if (!strcmp(schema->artifacts[i].id, schema->artifacts[j].id)) failed = 1;
        for (int j = 0; j < schema->artifacts[i].n_requires; j++) {
            int found = 0;
            for (int k = 0; k < schema->n_artifacts; k++)
                if (!strcmp(schema->artifacts[i].requires[j], schema->artifacts[k].id)) found = 1;
            if (!found) failed = 1;
        }
    }
    int complete[CHG_MAX_ARTIFACTS] = {0}, count = 0;
    for (int pass = 0; pass < schema->n_artifacts; pass++)
        for (int i = 0; i < schema->n_artifacts; i++) if (!complete[i] && chg_artifact_ready(schema, &schema->artifacts[i], complete)) { complete[i] = 1; count++; }
    free(copy);
    if (failed || !(seen & 1u) || !(seen & 8u) || !schema->n_artifacts || count != schema->n_artifacts)
        return cn_fail(store, "invalid_schema", EINVAL);
    return 0;
}
static int cn_schema_load(cn_store_t* store, const char* name, chg_schema_t* schema, cn_file_t* source) {
    char path[CN_PATH];
    if (!cn_name(name) || cn_format(path, sizeof path, "%s/%s/schema.yaml", CHG_SCHEMA_DIR, name))
        return cn_fail(store, "invalid_schema_name", EINVAL);
    if (cn_read(store, path, source)) return -1;
    if (!source->exists) return cn_fail(store, "schema_not_found", ENOENT);
    if (cn_schema_parse(store, source, schema)) return -1;
    if (strcmp(name, schema->name)) return cn_fail(store, "schema_name_mismatch", EINVAL);
    return 0;
}
static int cn_meta_parse(cn_store_t* store, const cn_file_t* source, cn_meta_t* meta) {
    memset(meta, 0, sizeof(*meta));
    if (!source->exists || (source->size && memchr(source->bytes, 0, source->size))) return cn_fail(store, "invalid_metadata", EINVAL);
    char* copy = strdup(source->bytes); if (!copy) return cn_fail(store, "allocation_failed", ENOMEM);
    char* save = NULL; int failed = 0;
    for (char* line = strtok_r(copy, "\n", &save); line && !failed; line = strtok_r(NULL, "\n", &save)) {
        if (!*line || *line == '#') continue;
        char* colon = strchr(line, ':'); if (!colon) { failed = 1; break; }
        *colon++ = 0; unsigned bit = 0; char* target = NULL; size_t capacity = 0;
        if (!strcmp(line, "uuid")) { bit = 1; target = meta->uuid; capacity = sizeof meta->uuid; }
        else if (!strcmp(line, "name")) { bit = 2; target = meta->name; capacity = sizeof meta->name; }
        else if (!strcmp(line, "schema")) { bit = 4; target = meta->schema; capacity = sizeof meta->schema; }
        else if (!strcmp(line, "status")) { bit = 8; target = meta->status; capacity = sizeof meta->status; }
        else if (!strcmp(line, "goal")) { bit = 16; target = meta->goal; capacity = sizeof meta->goal; }
        else if (!strcmp(line, "created")) {
            bit = 32; char number[64], *end;
            if (cn_scalar(colon, number, sizeof number) || !*number) { failed = 1; break; }
            errno = 0; long long value = strtoll(number, &end, 10);
            if (errno || *end || value < 0) failed = 1;
            meta->created = (int64_t)value;
        } else { failed = 1; break; }
        if (meta->seen & bit) failed = 1;
        meta->seen |= bit;
        if (target && cn_scalar(colon, target, capacity)) failed = 1;
    }
    free(copy);
    if (failed || (meta->seen & 47u) != 47u || !*meta->uuid || !cn_name(meta->name) || !cn_name(meta->schema) || !*meta->status)
        return cn_fail(store, "invalid_metadata", EINVAL);
    return 0;
}
static int cn_tree_markdown(cn_store_t* store, const char* path, unsigned depth, cn_names_t* files) {
    if (depth > 32u) return cn_fail(store, "spec_depth_limit", E2BIG);
    cn_names_t names; int exists;
    if (cn_list(store, path, &names, &exists)) return -1;
    int status = 0;
    for (size_t i = 0; i < names.count && !status; i++) {
        char child[CN_PATH]; struct stat st; int present;
        if (cn_format(child, sizeof child, "%s/%s", path, names.values[i])) { status = cn_fail(store, "spec_path_limit", errno); break; }
        if (cn_stat(store, child, &st, &present)) { status = -1; break; }
        if (!present) { status = cn_fail(store, "spec_tree_changed", ESTALE); break; }
        if (S_ISLNK(st.st_mode)) { status = cn_fail(store, "spec_symlink_refused", ELOOP); break; }
        if (S_ISDIR(st.st_mode)) status = cn_tree_markdown(store, child, depth + 1u, files);
        else {
            size_t length = strlen(names.values[i]);
            if (length > 3 && !strcmp(names.values[i] + length - 3, ".md")) {
                if (!S_ISREG(st.st_mode)) { status = cn_fail(store, "spec_not_regular", EINVAL); break; }
                if (files->count == CN_ENTRIES) { status = cn_fail(store, "spec_file_limit", E2BIG); break; }
                files->values[files->count] = strdup(child);
                if (!files->values[files->count]) { status = cn_fail(store, "allocation_failed", ENOMEM); break; }
                files->count++;
            }
        }
    }
    cn_names_free(&names); return status;
}
static int cn_change_load(cn_store_t* store, const char* path, cn_change_t* change) {
    memset(change, 0, sizeof(*change));
    char file[CN_PATH];
    if (cn_format(change->path, sizeof change->path, "%s", path) || cn_format(file, sizeof file, "%s/.change.yaml", path))
        return cn_fail(store, "change_path_limit", errno);
    if (cn_read(store, file, &change->metadata) || cn_meta_parse(store, &change->metadata, &change->meta)) return -1;
    cn_file_t source = {0};
    int status = cn_schema_load(store, change->meta.schema, &change->schema, &source);
    cn_file_free(&source); if (status) return -1;
    for (int i = 0; i < change->schema.n_artifacts; i++) {
        chg_artifact_t* artifact = &change->schema.artifacts[i];
        if (!strcmp(artifact->id, "specs")) {
            cn_names_t files = {0};
            if (cn_format(file, sizeof file, "%s/specs", path)) return cn_fail(store, "change_path_limit", errno);
            status = cn_tree_markdown(store, file, 0, &files); change->done[i] = files.count > 0; cn_names_free(&files);
            if (status) return -1;
        } else {
            struct stat st; int exists;
            if (cn_format(file, sizeof file, "%s/%s", path, artifact->generates)) return cn_fail(store, "change_path_limit", errno);
            if (cn_stat(store, file, &st, &exists)) return -1;
            if (exists && !S_ISREG(st.st_mode)) return cn_fail(store, "artifact_not_regular", EINVAL);
            change->done[i] = exists;
        }
    }
    return 0;
}
static int cn_active_names(cn_store_t* store, cn_names_t* selected) {
    memset(selected, 0, sizeof(*selected)); cn_names_t names; int exists;
    if (cn_list(store, CHG_DIR, &names, &exists)) return -1;
    int status = 0;
    for (size_t i = 0; i < names.count && !status; i++) {
        const char* name = names.values[i];
        if (!strcmp(name, "archive") || !strcmp(name, "schemas") || name[0] == '.') continue;
        char path[CN_PATH]; struct stat st; int present;
        if (!cn_name(name)) { status = cn_fail(store, "invalid_change_directory", EINVAL); break; }
        if (cn_format(path, sizeof path, "%s/%s/.change.yaml", CHG_DIR, name)) { status = cn_fail(store, "change_path_limit", errno); break; }
        if (cn_stat(store, path, &st, &present)) { status = -1; break; }
        if (!present) continue;
        if (!S_ISREG(st.st_mode)) { status = cn_fail(store, "metadata_not_regular", EINVAL); break; }
        selected->values[selected->count] = strdup(name);
        if (!selected->values[selected->count]) { status = cn_fail(store, "allocation_failed", ENOMEM); break; }
        selected->count++;
    }
    cn_names_free(&names); if (status) cn_names_free(selected); return status;
}
static int cn_resolve(cn_store_t* store, const char* requested, char name[CHG_MAX_NAME]) {
    if (requested) {
        if (!cn_name(requested)) return cn_fail(store, "invalid_change_name", EINVAL);
        memcpy(name, requested, strlen(requested) + 1u); return 0;
    }
    cn_names_t names;
    if (cn_active_names(store, &names)) return -1;
    int status = names.count == 1 ? 0 : cn_fail(store, names.count ? "ambiguous_change" : "change_not_found", names.count ? EINVAL : ENOENT);
    if (!status) memcpy(name, names.values[0], strlen(names.values[0]) + 1u);
    cn_names_free(&names); return status;
}
static int cn_append(char* output, size_t capacity, const void* bytes, size_t size) {
    size_t used = strlen(output);
    if (size >= capacity - used) { errno = E2BIG; return -1; }
    memcpy(output + used, bytes, size); output[used + size] = 0; return 0;
}
static int cn_spec_parse(cn_store_t* store, const cn_file_t* file, int delta, chg_spec_t* spec) {
    memset(spec, 0, sizeof(*spec));
    if (!file->exists) return delta ? cn_fail(store, "delta_not_found", ENOENT) : 0;
    if (file->size && memchr(file->bytes, 0, file->size)) return cn_fail(store, "invalid_spec", EINVAL);
    size_t offset = 0; int section = 0, current = -1, purpose = 0;
    while (offset < file->size) {
        size_t start = offset; while (offset < file->size && file->bytes[offset] != '\n') offset++;
        size_t length = offset - start; if (offset < file->size) offset++;
        if (length && file->bytes[start + length - 1] == '\r') length--;
        const char* line = file->bytes + start;
        if (length >= 3 && !memcmp(line, "## ", 3)) {
            purpose = !delta && length == 10 && !memcmp(line, "## Purpose", 10); current = -1;
            if (delta) section = length == 21 && !memcmp(line, "## ADDED Requirements", 21) ? 1 :
                length == 24 && !memcmp(line, "## MODIFIED Requirements", 24) ? 2 :
                length == 23 && !memcmp(line, "## REMOVED Requirements", 23) ? 3 : 0;
            continue;
        }
        if (length >= 16 && !memcmp(line, "### Requirement:", 16)) {
            if (delta && !section) return cn_fail(store, "delta_requirement_without_section", EINVAL);
            if (spec->n_reqs == CHG_MAX_REQS) return cn_fail(store, "spec_requirement_limit", E2BIG);
            size_t first = 16; while (first < length && (line[first] == ' ' || line[first] == '\t')) first++;
            while (length > first && (line[length - 1] == ' ' || line[length - 1] == '\t')) length--;
            if (length - first >= CHG_MAX_REQ_TITLE) return cn_fail(store, "spec_title_limit", E2BIG);
            current = spec->n_reqs++; purpose = 0; chg_requirement_t* requirement = &spec->reqs[current];
            memcpy(requirement->title, line + first, length - first); requirement->title[length - first] = 0;
            requirement->is_added = section == 1; requirement->is_modified = section == 2; requirement->is_removed = section == 3;
            for (int i = 0; i < current; i++) if (!strcmp(requirement->title, spec->reqs[i].title))
                return cn_fail(store, "duplicate_requirement", EINVAL);
            continue;
        }
        char* output = current >= 0 ? spec->reqs[current].body : purpose ? spec->purpose : NULL;
        size_t capacity = current >= 0 ? CHG_MAX_REQ_BODY : sizeof spec->purpose;
        if (output && (cn_append(output, capacity, line, length) || cn_append(output, capacity, "\n", 1)))
            return cn_fail(store, "spec_body_limit", E2BIG);
    }
    return 0;
}
static int cn_spec_bytes(cn_store_t* store, const chg_spec_t* spec, char** output, size_t* size) {
    *output = calloc(1, CN_FILE_BYTES + 1u); if (!*output) return cn_fail(store, "allocation_failed", ENOMEM);
    int failed = cn_append(*output, CN_FILE_BYTES + 1u, "# Specification\n\n## Purpose\n\n", 29);
    const char* purpose = *spec->purpose ? spec->purpose : "_Describe this domain._\n";
    failed |= cn_append(*output, CN_FILE_BYTES + 1u, purpose, strlen(purpose));
    failed |= cn_append(*output, CN_FILE_BYTES + 1u, "\n## Requirements\n\n", 18);
    for (int i = 0; i < spec->n_reqs && !failed; i++) {
        const chg_requirement_t* r = &spec->reqs[i];
        failed |= cn_append(*output, CN_FILE_BYTES + 1u, "### Requirement: ", 17);
        failed |= cn_append(*output, CN_FILE_BYTES + 1u, r->title, strlen(r->title));
        failed |= cn_append(*output, CN_FILE_BYTES + 1u, "\n", 1);
        failed |= cn_append(*output, CN_FILE_BYTES + 1u, r->body, strlen(r->body));
        failed |= cn_append(*output, CN_FILE_BYTES + 1u, "\n", 1);
    }
    if (failed) { free(*output); *output = NULL; return cn_fail(store, "spec_output_limit", E2BIG); }
    *size = strlen(*output); return 0;
}
