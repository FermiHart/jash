// SPDX-License-Identifier: LicenseRef-SPL-1.0
// Report-local contracts. Real redirects/budgets are covered by the Python CLI gate.
#include <core/mutation_report.h>
#include <core/data_wire.h>
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

void jash_budget_unwrap(jash_writer_t* writer, void** context) { (void)writer; (void)context; }
void jash_redir_stream_writer(jash_writer_t writer, void* context) { (void)writer; (void)context; }

typedef struct { char data[8192]; size_t size, limit; } sink_t;
static long sink(void* context, const char* data, size_t n) {
    sink_t* s = context;
    if (s->size >= s->limit) { errno = EPIPE; return -1; }
    if (n > s->limit - s->size) n = s->limit - s->size;
    if (n > 7) n = 7; /* Exercise short writes, including a failure mid-item. */
    assert(s->size + n < sizeof(s->data));
    memcpy(s->data + s->size, data, n);
    s->size += n;
    return (long)n;
}

static void pipe_handler(int sig) { (void)sig; }
static void check_restored(void) {
    struct sigaction current;
    assert(sigaction(SIGPIPE, NULL, &current) == 0);
    assert(current.sa_handler == pipe_handler);
    assert(sigaction(SIGTERM, NULL, &current) == 0 && current.sa_handler == pipe_handler);
}

static void native_contracts(void) {
    static const char* const values[] = {"-t", "--target-directory", NULL};
    char* input[] = {"--data-v2", "-vt", "--json", "--", "--data", NULL};
    char** output; int count; jash_mutation_format_t format;
    assert(jash_mutation_args_mode(5, input, values, &count, &output, &format) == 1);
    assert(format == JASH_MUTATION_DATA && count == 4);
    assert(!strcmp(output[0], "-vt") && !strcmp(output[1], "--json") && !strcmp(output[3], "--data"));
    free(output);
    char* conflicting[] = {"--json", "--data", "operand", NULL};
    assert(jash_mutation_args_mode(3, conflicting, NULL, &count, &output, &format) == -1);
    assert(errno == EINVAL && output == NULL && format == JASH_MUTATION_JSON);
    char* literal[] = {"operand", "--data", NULL};
    assert(jash_mutation_args_mode(2, literal, NULL, &count, &output, &format) == 0 && output == NULL);
    assert(jash_mutation_cancelable("mkdir", 5, input));

    jash_mutation_report_t report; sink_t s = {.limit = 0};
    assert(jash_mutation_report_begin_mode(&report, "cp", "filesystem_action", JASH_MUTATION_DATA, sink, &s) == -1);
    check_restored();
    s.limit = sizeof(s.data) - 1;
    assert(jash_mutation_report_begin_mode(&report, "cp", "filesystem_action", JASH_MUTATION_DATA, sink, &s) == 0);
    assert(s.size > 4 && !memcmp(s.data, "JTV2", 4));
    assert(jash_mutation_report_heartbeat(&report) == 0 && report.items == 0);
    jash_mutation_item_t item = {.source = "src-\xff", .destination = "dst", .action = "copy",
        .status = JASH_MUT_COMPLETED, .effect = JASH_MUT_CHANGED,
        .bytes_written = UINT64_MAX, .bytes_known = 1, .verification = 1};
    assert(jash_mutation_report_item(&report, &item) == 0);
    assert(jash_mutation_report_end(&report, 0) == 0);
    assert(report.items == 1 && report.completed == 1);
    check_restored();
    unsigned char storage[65536]; jdv_arena_t arena; jdv_arena_init(&arena, storage, sizeof(storage));
    jdv_wire_limits_t limits; jdv_wire_limits_init(&limits); jdv_wire_info_t info;
    assert(jdv_wire_validate_v2(s.data, s.size, &arena, &limits, &info) == 0);
    assert(info.items_seen == 4 && info.bytes_seen == UINT64_MAX && info.logical_verified && info.final_hash_present);
    assert(info.state == JDV_STREAM_COMPLETE);

    s.size = 0;
    assert(jash_mutation_report_begin_mode(&report, "cp", "filesystem_action", JASH_MUTATION_DATA, sink, &s) == 0);
    item.status = JASH_MUT_FAILED; item.effect = JASH_MUT_POSSIBLE;
    assert(jash_mutation_report_item(&report, &item) == 0);
    assert(jash_mutation_report_end(&report, 1) == 1);
    assert(jdv_wire_validate_v2(s.data, s.size, &arena, &limits, &info) == 0);
    assert(info.state == JDV_STREAM_FAILED && info.completeness == JDV_COMPLETENESS_PARTIAL && info.errors_seen == 1);
    check_restored();

    s.size = 0;
    assert(jash_mutation_report_begin_mode(&report, "rm", "operand_tree", JASH_MUTATION_DATA, sink, &s) == 0);
    assert(raise(SIGTERM) == 0 && jash_mutation_report_stop(&report) == 143);
    assert(jash_mutation_report_end(&report, 0) == 143);
    assert(jdv_wire_validate_v2(s.data, s.size, &arena, &limits, &info) == 0);
    assert(info.state == JDV_STREAM_CANCELLED && info.items_seen == 2);
    check_restored();

    s.size = 0;
    assert(jash_mutation_report_begin_mode(&report, "cp", "filesystem_action", JASH_MUTATION_DATA, sink, &s) == 0);
    s.limit = s.size + 19;
    assert(jash_mutation_report_item(&report, &item) == -1);
    assert(jash_mutation_report_end(&report, 1) == 125);
    assert(jdv_wire_validate_v2(s.data, s.size, &arena, &limits, &info) == -1);
    check_restored();
}

int main(void) {
    static const char* const values[] = {"-t", "--target-directory", NULL};
    char* input[] = {"--json", "-vt", "--json", "--", "--json", NULL};
    char** output;
    int count;
    assert(jash_mutation_args(5, input, values, &count, &output) == 1);
    assert(count == 4 && output[4] == NULL);
    assert(!strcmp(output[0], "-vt") && !strcmp(output[1], "--json"));
    assert(!strcmp(output[2], "--") && !strcmp(output[3], "--json"));
    free(output);
    assert(jash_mutation_args(4, input + 1, values, &count, &output) == 0 && output == NULL);

    struct sigaction action = {0};
    action.sa_handler = pipe_handler;
    sigemptyset(&action.sa_mask);
    assert(sigaction(SIGPIPE, &action, NULL) == 0);
    assert(sigaction(SIGTERM, &action, NULL) == 0);
    native_contracts();
    jash_mutation_report_t report;
    sink_t s = {.limit = 0};
    assert(jash_mutation_report_begin(&report, "cp", "filesystem_action", sink, &s) != 0);
    check_restored();

    s.limit = sizeof(s.data) - 1;
    assert(jash_mutation_report_begin(&report, "cp", "filesystem_action", sink, &s) == 0);
    s.limit = s.size + 19;
    jash_mutation_item_t item = {.source = "src", .destination = "dst", .action = "copy",
                                .status = JASH_MUT_COMPLETED, .effect = JASH_MUT_CHANGED};
    assert(jash_mutation_report_item(&report, &item) != 0);
    assert(jash_mutation_report_failed(&report));
    size_t failed_size = s.size;
    assert(jash_mutation_report_item(&report, &item) != 0 && s.size == failed_size);
    assert(jash_mutation_report_end(&report, 0) == 125);
    check_restored();

    s.size = 0; s.limit = sizeof(s.data) - 1;
    assert(jash_mutation_report_begin(&report, "cp", "filesystem_action", sink, &s) == 0);
    item.status = (jash_mutation_status_t)-1;
    assert(jash_mutation_report_item(&report, &item) != 0);
    assert(jash_mutation_report_end(&report, 0) == 125);
    check_restored();

    s.size = 0;
    assert(jash_mutation_report_begin(&report, "rm", "operand_tree", sink, &s) == 0);
    assert(raise(SIGTERM) == 0);
    assert(jash_mutation_cancel_status() == 143);
    assert(jash_mutation_report_stop(&report) == 143);
    assert(jash_mutation_report_end(&report, 0) == 143);
    check_restored();
    assert(jash_mutation_cancel_status() == 0);
    s.size = 0;
    assert(jash_mutation_report_begin(&report, "cp", "filesystem_action", sink, &s) == 0);
    item.status = JASH_MUT_FAILED;
    item.effect = JASH_MUT_POSSIBLE;
    item.source = "raw-\xff";
    item.code = "write_destination";
    item.bytes_written = UINT64_MAX;
    item.bytes_known = 1;
    assert(jash_mutation_report_item(&report, &item) == 0);
    assert(jash_mutation_report_end(&report, 1) == 1);
    check_restored();
    assert(fwrite(s.data, 1, s.size, stdout) == s.size);
    return 0;
}
