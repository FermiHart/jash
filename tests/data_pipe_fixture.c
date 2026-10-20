// SPDX-License-Identifier: LicenseRef-SPL-1.0
/* Native fixtures deliberately distinguish types that share a JSON projection. */
#include "core/data_emit.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static long output(void* context, const void* bytes, size_t size) {
    (void)context;
    size_t written = fwrite(bytes, 1, size, stdout);
    if (!written && size) { if (!errno) errno = EIO; return -1; }
    return (long)written;
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    unsigned char storage[65536], workspace[16384];
    jdv_arena_t arena; jdv_arena_init(&arena, storage, sizeof(storage));
    jdv_value_t rows[16]; uint64_t credits[16]; size_t count = 0;
    jdv_stream_end_t outcome; if (jdv_stream_end_init(&outcome)) return 2;
    outcome.state = JDV_STREAM_COMPLETE; outcome.completeness = JDV_COMPLETENESS_COMPLETE;
    if (!strcmp(argv[1], "groups")) {
        jdv_value_t keys[9] = {jdv_value_filesize(1), jdv_value_u64(1), jdv_value_i64(1)};
        if (jdv_value_string(&arena, "x", 1, &keys[3]) ||
            jdv_value_symbol(&arena, "x", 1, &keys[4]) ||
            jdv_value_path(&arena, "x", 1, &keys[5]) ||
            jdv_value_bytes(&arena, "x", 1, &keys[6])) return 2;
        jdv_record_input_t tagged[2] = {{.key = JDV_LITERAL("encoding")}, {.key = JDV_LITERAL("data")}};
        if (jdv_value_string(&arena, "base64", 6, &tagged[0].value) ||
            jdv_value_string(&arena, "eA==", 4, &tagged[1].value) ||
            jdv_value_record(&arena, tagged, 2, &keys[7])) return 2;
        keys[8] = keys[6];
        count = 9;
        for (size_t i = 0; i < count; i++) {
            jdv_record_input_t f = {JDV_LITERAL("k"), keys[i]};
            if (jdv_value_record(&arena, &f, 1, &rows[i])) return 2;
            credits[i] = i + 1;
        }
    } else {
        count = 2;
        for (size_t i = 0; i < count; i++) {
            jdv_record_input_t fields[2] = {{.key = JDV_LITERAL("n")}, {.key = JDV_LITERAL("name")}};
            fields[0].value = jdv_value_filesize(i ? 5 : 3);
            if (!strcmp(argv[1], "duration")) fields[0].value = jdv_value_duration(i ? 3 : -7);
            if (!strcmp(argv[1], "timestamp")) fields[0].value = jdv_value_timestamp(i ? 5 : 3);
            if (!strcmp(argv[1], "mixedunits") && i) fields[0].value = jdv_value_u64(3);
            if (!strcmp(argv[1], "fractional")) fields[0].value = jdv_value_filesize(i ? 2 : 1);
            if (!strcmp(argv[1], "fractional-duration")) fields[0].value = jdv_value_duration(i ? 0 : -1);
            if (!strcmp(argv[1], "fractional-timestamp")) fields[0].value = jdv_value_timestamp(i ? -1 : -2);
            if (!strcmp(argv[1], "avg-unsigned-limit")) fields[0].value = jdv_value_u64(i ? UINT64_MAX - 1u : UINT64_MAX);
            if (!strcmp(argv[1], "avg-signed-limit")) fields[0].value = jdv_value_i64(i ? INT64_MIN + 1 : INT64_MIN);
            if (!strcmp(argv[1], "avg-mixed-int")) fields[0].value = i ? jdv_value_u64(UINT64_MAX) : jdv_value_i64(INT64_MIN);

            if (jdv_value_bytes(&arena, i ? "a" : "z", 1, &fields[1].value) ||
                jdv_value_record(&arena, fields, 2, &rows[i])) return 2;
            credits[i] = (i + 1) * 100;
        }
        if (!strcmp(argv[1], "failed") || !strcmp(argv[1], "incomplete") || !strcmp(argv[1], "cancelled")) {
            outcome.state = !strcmp(argv[1], "failed") ? JDV_STREAM_FAILED :
                !strcmp(argv[1], "incomplete") ? JDV_STREAM_INCOMPLETE : JDV_STREAM_CANCELLED;
            outcome.completeness = JDV_COMPLETENESS_PARTIAL;
            outcome.reason = JDV_LITERAL("fixture partial");
            outcome.producer_status = 7;
            outcome.truncated = outcome.state == JDV_STREAM_INCOMPLETE;
        }
        if (!strcmp(argv[1], "errors")) outcome.errors_seen = 1;
        if (!strcmp(argv[1], "status")) outcome.producer_status = 7;
    }
    jdv_schema_t any; if (jdv_schema_any(&any)) return 2;
    jdv_stream_config_t config; jdv_stream_config_init(&config);
    config.boundedness = JDV_STREAM_FINITE;
    config.declared_schema = config.guaranteed_schema = &any;
    jdv_stream_end_t final;
    if (jdv_emit_sequence(rows, count, credits, &config, &outcome,
            output, NULL, workspace, sizeof(workspace), &final)) {
        fprintf(stderr, "native fixture: %s\n", strerror(errno)); return 2;
    }
    return fflush(stdout) ? 2 : 0;
}
