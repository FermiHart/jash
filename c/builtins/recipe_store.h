// SPDX-License-Identifier: LicenseRef-SPL-1.0
/* Native recipe observations use the existing line format, with checked
 * limits and complete snapshots. No command runs while reading a recipe. */
#ifndef JASH_BUILTINS_RECIPE_STORE_H
#define JASH_BUILTINS_RECIPE_STORE_H
#include "memory_chain.h"
#include "semantic_events.h"
#include <core/query_cancel.h>
#include <inttypes.h>
#include <dirent.h>

#define RECIPE_NAME_MAX 56u /* "recipe-" plus this name fits checkpoint's 63 bytes. */
#define RECIPE_COMMAND_MAX 1023u
#define RECIPE_PARAM_MAX 16u
#define RECIPE_STEPS_MAX 256u
#define RECIPE_LIST_MAX 512u
#define RECIPE_SCAN_BYTES (16u * 1024u * 1024u)

typedef struct {
    char name[RECIPE_NAME_MAX + 1u], session[512];
    int64_t created;
    uint64_t from, to;
    char parameters[RECIPE_PARAM_MAX][64]; size_t parameter_count;
    char commands[RECIPE_STEPS_MAX][RECIPE_COMMAND_MAX + 1u];
    char kinds[RECIPE_STEPS_MAX]; size_t step_count;
    char path[PATH_MAX]; char* raw; size_t size;
    int present, directory_present, error;
} recipe_document_t;
typedef struct { jash_state_t* state; int cancelled; } recipe_control_t;

static int recipe_poll(void* context) {
    recipe_control_t* control = context;
    if (!control->cancelled) control->cancelled = jash_query_cancel_poll(control->state);
    return control->cancelled;
}

static int recipe_name_valid(const char* name) {
    size_t length = strlen(name);
    if (!length || length > RECIPE_NAME_MAX || !strcmp(name, ".") || !strcmp(name, "..")) return 0;
    for (size_t i = 0; i < length; i++) {
        unsigned char ch = (unsigned char)name[i];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.')) return 0;
    }
    return 1;
}

static int recipe_parameter_valid(const char* name, size_t size) {
    if (!size || size > 63u) return 0;
    for (size_t i = 0; i < size; i++) {
        unsigned char ch = (unsigned char)name[i];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_' ||
              (i && ch >= '0' && ch <= '9'))) return 0;
    }
    return 1;
}

/* Replacement is all-or-error. The previous would-have-written snprintf
 * accumulation could overrun the terminator when replacement text expanded. */
static int recipe_replace(const char* source, const char* needle, const char* replacement,
                           char* output, size_t capacity) {
    size_t match = strlen(needle), insert = strlen(replacement), used = 0;
    if (!capacity) return -1;
    for (const char* cursor = source; *cursor;) {
        int matched = match && !strncmp(cursor, needle, match);
        size_t take = matched ? insert : 1u;
        const char* bytes = matched ? replacement : cursor;
        size_t advance = matched ? match : 1u;
        if (take >= capacity - used) { output[0] = 0; return -1; }
        memcpy(output + used, bytes, take); used += take; cursor += advance;
    }
    output[used] = 0; return 0;
}

static void recipe_document_free(recipe_document_t* recipe) {
    free(recipe->raw); recipe->raw = NULL;
}

static int recipe_document_parse(recipe_document_t* recipe) {
    if (!recipe->present) return 0;
    if (!recipe->size || recipe->raw[recipe->size - 1u] != '\n' ||
        memchr(recipe->raw, 0, recipe->size) || memchr(recipe->raw, '\r', recipe->size)) return EINVAL;
    char* parsed = malloc(recipe->size + 1u);
    if (!parsed) return ENOMEM;
    memcpy(parsed, recipe->raw, recipe->size + 1u);
    unsigned headers = 0; int error = 0;
    for (char* cursor = parsed; *cursor && !error;) {
        char* line = cursor; char* end = strchr(line, '\n');
        if (!end) { error = EINVAL; break; }
        *end = 0; cursor = end + 1;
        if (!strncmp(line, "name ", 5)) {
            if ((headers & 1u) || strcmp(line + 5, recipe->name)) error = EINVAL;
            headers |= 1u;
        } else if (!strncmp(line, "created ", 8)) {
            uint64_t timestamp = 0;
            if ((headers & 2u) || memory_unsigned(line + 8, INT64_MAX, &timestamp)) error = EINVAL;
            recipe->created = (int64_t)timestamp; headers |= 2u;
        } else if (!strncmp(line, "session ", 8)) {
            if ((headers & 4u) || !line[8] || strlen(line + 8) >= sizeof(recipe->session)) error = EINVAL;
            else memcpy(recipe->session, line + 8, strlen(line + 8) + 1u);
            headers |= 4u;
        } else if (!strncmp(line, "range ", 6)) {
            char* space = strchr(line + 6, ' ');
            if ((headers & 8u) || !space) error = EINVAL;
            else {
                *space++ = 0;
                if (memory_unsigned(line + 6, UINT64_MAX, &recipe->from) ||
                    memory_unsigned(space, UINT64_MAX, &recipe->to) || recipe->from > recipe->to) error = EINVAL;
            }
            headers |= 8u;
        } else if (!strncmp(line, "param ", 6)) {
            size_t length = strlen(line + 6);
            if (recipe->step_count || recipe->parameter_count == RECIPE_PARAM_MAX) error = E2BIG;
            else if (!recipe_parameter_valid(line + 6, length)) error = EINVAL;
            else {
                for (size_t i = 0; i < recipe->parameter_count; i++)
                    if (!strcmp(recipe->parameters[i], line + 6)) error = EINVAL;
                if (!error) memcpy(recipe->parameters[recipe->parameter_count++], line + 6, length + 1u);
            }
        } else if ((line[0] == 'C' || line[0] == 'A') && line[1] == ' ') {
            size_t length = strlen(line + 2);
            if (recipe->step_count == RECIPE_STEPS_MAX || length > RECIPE_COMMAND_MAX) error = E2BIG;
            else if (headers != 15u || !length) error = EINVAL;
            else {
                recipe->kinds[recipe->step_count] = line[0];
                memcpy(recipe->commands[recipe->step_count++], line + 2, length + 1u);
            }
        } else error = EINVAL;
    }
    if (!error && (headers != 15u || !recipe->step_count)) error = EINVAL;
    free(parsed); return error;
}

static int recipe_document_read(recipe_document_t* recipe, const char* name) {
    memset(recipe, 0, sizeof(*recipe));
    if (!recipe_name_valid(name)) { recipe->error = EINVAL; return 2; }
    memcpy(recipe->name, name, strlen(name) + 1u);
    const char* home = getenv("HOME");
    int n = snprintf(recipe->path, sizeof(recipe->path), "%s/.jash/recipes/%s.jashrc", home ? home : "", name);
    if (n < 0 || (size_t)n >= sizeof(recipe->path)) { recipe->error = E2BIG; return 75; }
    char filename[80]; snprintf(filename, sizeof(filename), "%s.jashrc", name);
    memory_store_t store = {-1, -1};
    if (memory_store_open(&store, "recipes", 0)) recipe->error = errno;
    else {
        recipe->directory_present = store.directory >= 0;
        if (memory_store_read(&store, filename, MEMORY_STORE_MAX_BYTES, &recipe->raw,
                              &recipe->size, &recipe->present)) recipe->error = errno;
    }
    if (memory_store_close(&store) && !recipe->error) recipe->error = errno;
    if (!recipe->error) recipe->error = recipe_document_parse(recipe);
    return recipe->error ? recipe->error == E2BIG ? 75 : 1 : 0;
}

static jdv_value_t recipe_source(semantic_value_t* sem, const recipe_document_t* recipe) {
    jdv_record_input_t fields[] = {
        semantic_field("path", semantic_path(sem, recipe->path)),
        semantic_field("present", jdv_value_bool(recipe->present)),
        semantic_field("read_complete", jdv_value_bool(!recipe->error)),
        semantic_field("bytes", jdv_value_filesize(recipe->size)),
        semantic_field("format", semantic_symbol(sem, "legacy-recipe-v1")),
        semantic_field("error", semantic_symbol(sem, recipe->error ? memory_store_error(recipe->error) : NULL)),
        semantic_field("errno", recipe->error ? jdv_value_i64(recipe->error) : jdv_value_nothing())
    };
    return semantic_record(sem, fields, 7);
}

static jdv_value_t recipe_value(semantic_value_t* sem, const recipe_document_t* recipe, int include_steps) {
    jdv_value_t parameters[RECIPE_PARAM_MAX], steps[RECIPE_STEPS_MAX];
    for (size_t i = 0; i < recipe->parameter_count; i++) parameters[i] = semantic_symbol(sem, recipe->parameters[i]);
    for (size_t i = 0; include_steps && i < recipe->step_count; i++) {
        jdv_record_input_t step[] = {
            semantic_field("index", jdv_value_u64(i + 1u)),
            semantic_field("kind", semantic_symbol(sem, recipe->kinds[i] == 'A' ? "assertion" : "command")),
            semantic_field("command", semantic_text(sem, recipe->commands[i]))
        };
        steps[i] = semantic_record(sem, step, 3);
    }
    jdv_record_input_t range[] = {semantic_field("from", jdv_value_u64(recipe->from)), semantic_field("to", jdv_value_u64(recipe->to))};
    jdv_record_input_t fields[] = {
        semantic_field("name", semantic_symbol(sem, recipe->name)),
        semantic_field("created_seconds", jdv_value_i64(recipe->created)),
        semantic_field("session", semantic_text(sem, recipe->session)),
        semantic_field("range", semantic_record(sem, range, 2)),
        semantic_field("parameters", semantic_list(sem, parameters, recipe->parameter_count)),
        semantic_field("step_count", jdv_value_u64(recipe->step_count)),
        semantic_field("steps", include_steps ? semantic_list(sem, steps, recipe->step_count) : jdv_value_nothing()),
        semantic_field("source", recipe_source(sem, recipe))
    };
    return semantic_record(sem, fields, include_steps == 2 ? 7u : 8u);
}

static int recipe_serialize(const recipe_document_t* recipe, jash_structured_buffer_t* bytes) {
    jash_structured_buffer_init(bytes, MEMORY_STORE_MAX_BYTES);
    char header[1024];
    int n = snprintf(header, sizeof(header), "name %s\ncreated %" PRId64 "\nsession %s\nrange %" PRIu64 " %" PRIu64 "\n",
                     recipe->name, recipe->created, recipe->session, recipe->from, recipe->to);
    if (n < 0 || (size_t)n >= sizeof(header) || jash_structured_buffer_sink(bytes, header, (size_t)n) < 0) return -1;
    for (size_t i = 0; i < recipe->parameter_count; i++) {
        n = snprintf(header, sizeof(header), "param %s\n", recipe->parameters[i]);
        if (n < 0 || (size_t)n >= sizeof(header) || jash_structured_buffer_sink(bytes, header, (size_t)n) < 0) return -1;
    }
    for (size_t i = 0; i < recipe->step_count; i++) {
        char prefix[2] = {recipe->kinds[i], ' '};
        if (jash_structured_buffer_sink(bytes, prefix, 2) < 0 ||
            jash_structured_buffer_sink(bytes, recipe->commands[i], strlen(recipe->commands[i])) < 0 ||
            jash_structured_buffer_sink(bytes, "\n", 1) < 0) return -1;
    }
    return 0;
}

static memory_write_t recipe_publish(const recipe_document_t* before, const jash_structured_buffer_t* bytes,
                                     int remove, recipe_control_t* control) {
    memory_write_t result = {0}; memory_store_t store = {-1, -1};
    char filename[80]; snprintf(filename, sizeof(filename), "%s.jashrc", before->name);
    if (recipe_poll(control)) result.error = ECANCELED;
    else if (memory_store_open_cooperative(&store, "recipes", 1, recipe_poll, control)) result.error = errno;
    char* current = NULL; size_t size = 0; int present = 0;
    if (!result.error && memory_store_read(&store, filename, MEMORY_STORE_MAX_BYTES, &current, &size, &present)) result.error = errno;
    if (!result.error && (present != before->present || size != before->size ||
        (size && memcmp(current, before->raw, size)))) result.error = ESTALE;
    free(current);
    if (!result.error && recipe_poll(control)) result.error = ECANCELED;
    if (!result.error) result = remove ? memory_store_remove(&store, filename) :
        memory_store_publish(&store, filename, bytes->data, bytes->used);
    if (memory_store_close(&store) && !result.error) result.error = errno;
    return result;
}

static jdv_value_t recipe_publication(semantic_value_t* sem, const recipe_document_t* recipe,
                                     const memory_write_t* result, size_t bytes, int worst) {
    jdv_record_input_t fields[] = {
        semantic_field("path", semantic_path(sem, recipe->path)),
        semantic_field("applied", jdv_value_bool(result->applied)),
        semantic_field("durable", jdv_value_bool(result->durable)),
        semantic_field("bytes_written", result->applied ? jdv_value_filesize(bytes) : jdv_value_nothing()),
        semantic_field("error", semantic_symbol(sem, worst ? "recipe_directory_sync_failed" : result->error ? memory_store_error(result->error) : NULL)),
        semantic_field("errno", result->error ? jdv_value_i64(result->error) : jdv_value_nothing())
    };
    return semantic_record(sem, fields, 6);
}
#endif
