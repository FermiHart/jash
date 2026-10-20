// SPDX-License-Identifier: LicenseRef-SPL-1.0
#include "structured_output.h"
#include "output_budget.h"
#include <parser/builtin_redir.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int jash_structured_begin(jash_structured_output_t* out,
                          jash_writer_t writer, void* context) {
    memset(out, 0, sizeof(*out));
    struct sigaction ignore = {0};
    ignore.sa_handler = SIG_IGN;
    sigemptyset(&ignore.sa_mask);
    if (sigaction(SIGPIPE, &ignore, &out->previous_sigpipe)) return -1;
    out->restore_sigpipe = 1;
    jash_budget_unwrap(&writer, &context);
    jash_redir_stream_writer(writer, context);
    jash_jsonw_init(&out->json, writer, context);
    return 0;
}

int jash_structured_finish(jash_structured_output_t* out, int status) {
    int failed = jash_jsonw_finish(&out->json);
    if (out->restore_sigpipe) {
        if (sigaction(SIGPIPE, &out->previous_sigpipe, NULL)) failed = -1;
        out->restore_sigpipe = 0;
    }
    return failed ? 125 : status;
}

int jash_structured_error(jash_writer_t writer, void* context,
                          const char* command, const char* code, int status) {
    jash_structured_output_t out;
    if (jash_structured_begin(&out, writer, context)) return 125;
    jash_jsonw_raw(&out.json, "{\"schema\":\"jash.builtin.error.v1\",\"command\":");
    jash_jsonw_string(&out.json, command);
    jash_jsonw_raw(&out.json, ",\"complete\":false,\"error\":{\"code\":");
    jash_jsonw_string(&out.json, code);
    jash_jsonw_raw(&out.json, "}}\n");
    return jash_structured_finish(&out, status);
}

void jash_structured_buffer_init(jash_structured_buffer_t* b, size_t limit) {
    memset(b, 0, sizeof(*b));
    b->limit = limit;
}

long jash_structured_buffer_sink(void* context, const char* data, size_t n) {
    jash_structured_buffer_t* b = context;
    if (b->error) { errno = b->error; return -1; }
    if (n > SIZE_MAX - b->required) { b->error = EOVERFLOW; errno = EOVERFLOW; return -1; }
    b->required += n;
    if (b->required > b->limit) return (long)n;
    size_t need = b->used + n + 1;
    if (need > b->capacity) {
        size_t capacity = b->capacity ? b->capacity : 1024;
        while (capacity < need && capacity <= SIZE_MAX / 2) capacity *= 2;
        if (capacity > b->limit + 1) capacity = b->limit + 1;
        if (capacity < need) { b->error = EOVERFLOW; errno = EOVERFLOW; return -1; }
        char* next = realloc(b->data, capacity);
        if (!next) { b->error = ENOMEM; return -1; }
        b->data = next;
        b->capacity = capacity;
    }
    memcpy(b->data + b->used, data, n);
    b->used += n;
    b->data[b->used] = '\0';
    return (long)n;
}

void jash_structured_buffer_free(jash_structured_buffer_t* b) {
    free(b->data);
    memset(b, 0, sizeof(*b));
}

int jash_structured_document(jash_writer_t writer, void* context,
                             const char* command, const char* document,
                             size_t available, size_t required,
                             long budget, int command_status) {
    jash_structured_output_t out;
    if (jash_structured_begin(&out, writer, context)) return 125;
    int over_budget = budget > 0 && required > (size_t)budget;
    if (over_budget || required > available) {
        jash_jsonw_raw(&out.json, "{\"schema\":\"jash.builtin.error.v1\",\"command\":");
        jash_jsonw_string(&out.json, command);
        jash_jsonw_raw(&out.json, ",\"complete\":false,\"command_status\":");
        jash_jsonw_i64(&out.json, command_status);
        jash_jsonw_raw(&out.json, ",\"error\":{\"code\":");
        jash_jsonw_string(&out.json, over_budget ? "output_budget_exceeded" : "output_limit_exceeded");
        jash_jsonw_raw(&out.json, ",\"required_bytes\":");
        jash_jsonw_safe_u64(&out.json, required);
        jash_jsonw_raw(&out.json, ",\"budget_bytes\":");
        jash_jsonw_safe_u64(&out.json, budget > 0 ? (uint64_t)budget : available);
        jash_jsonw_raw(&out.json, "}}\n");
        return jash_structured_finish(&out, 75);
    }
    if (!document || strlen(document) != available) {
        out.json.error = EINVAL;
        return jash_structured_finish(&out, 125);
    }
    jash_jsonw_raw(&out.json, document);
    return jash_structured_finish(&out, command_status);
}
