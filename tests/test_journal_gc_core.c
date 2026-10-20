// SPDX-License-Identifier: LicenseRef-SPL-1.0
#define _GNU_SOURCE
#define JASH_AGENT_ONLY
#include "../c/core/serve.c"
#include <assert.h>

int main(int argc, char** argv) {
    assert(argc == 2 && !setenv("HOME", argv[1], 1));
    assert(!srv_store_bind() && !flock(srv_store_lease, LOCK_EX));
    srv_receipt_signer_t signer; assert(!srv_receipt_signer_load(&signer));
    const char* text = "{\"id\":\"native-gc\",\"protocol_version\":2,\"op\":\"store.quota\",\"max_bytes\":0,\"caps\":[\"store:manage\"]}";
    srv_job_req_t* request = calloc(1, sizeof(*request)); assert(request && !srv_job_parse(text, request));
    srv_job_request_digest(text, request->request_digest);
    maintenance_tx_t tx = {0}; maintenance_key(request->id_value, tx.key);
    assert(!maintenance_begin(&tx, request, text, &signer));
    maintenance_buffer_t buffer = {0}; buffer.bytes = malloc(STORE_MAINT_MAX); assert(buffer.bytes);
    const char* body = "{\"schema\":\"jash.agent-store.v1\",\"id\":\"native-gc\",\"op\":\"store.quota\",\"effect_admission\":\"admitted\",\"transaction\":\"committed\",\"ok\":true,\"applied\":false}\n";
    strcpy(buffer.bytes, body); buffer.used = strlen(body);
    assert(!maintenance_commit(&tx, request, &buffer, &signer));
    char staged[128], held[128], original[256], key[65];
    snprintf(staged, sizeof(staged), ".%s.result.1.1.tmp", tx.key);
    snprintf(held, sizeof(held), ".jgc-%s", staged + 1);
    assert(maintenance_gc_name(staged, original, key) == 0 && !strcmp(key, tx.key));
    assert(maintenance_gc_name(held, original, key) == 1 && !strcmp(original, staged));
    assert(maintenance_gc_name(".jgc-invalid", original, key) < 0);
    assert(!maintenance_publish(staged, buffer.bytes, buffer.used, 0));
    assert(!maintenance_publish(".unique", "unique", 6, 0));
    int fd = open("/dev/null", O_WRONLY); assert(fd >= 0);
    jash_json_writer_t writer; jash_jsonw_init(&writer, jash_jsonw_fd_sink, &fd);
    request->dry_run = 1; request->older_than_seconds = 0;
    journal_gc_report_t report = {.verified_bytes = 2u * SRV_STORE_SCAN_BYTES};
    assert(maintenance_journal_gc(&report, request, &signer, &writer));
    assert(!report.reclaimed && !report.planned && !report.changed);
    assert(!strcmp(report.entries[0].action, "retained_verification_limit")); free(report.entries);
    memset(&report, 0, sizeof(report));
    assert(!maintenance_journal_gc(&report, request, &signer, &writer));
    assert(report.complete && report.planned == 1 && !report.changed); free(report.entries);
    request->dry_run = 0; memset(&report, 0, sizeof(report));
    assert(!maintenance_journal_gc(&report, request, &signer, &writer));
    assert(report.reclaimed == 1 && report.reclaimed_bytes == buffer.used && report.changed); free(report.entries);
    int dir = srv_store_dir("agent_maintenance"); assert(dir >= 0);
    struct stat st; assert(fstatat(dir, staged, &st, AT_SYMLINK_NOFOLLOW) && errno == ENOENT);
    assert(!fstatat(dir, ".unique", &st, AT_SYMLINK_NOFOLLOW));
    /* A held duplicate is subject to the same proof, never age-only deletion. */
    assert(!maintenance_publish(held, buffer.bytes, buffer.used, 0));
    memset(&report, 0, sizeof(report));
    assert(!maintenance_journal_gc(&report, request, &signer, &writer));
    assert(report.reclaimed == 1); free(report.entries);
    assert(!maintenance_publish(staged, "different", 9, 0));
    memset(&report, 0, sizeof(report));
    assert(!maintenance_journal_gc(&report, request, &signer, &writer));
    assert(!report.reclaimed && !report.changed); free(report.entries);
    char* replay = NULL; assert(maintenance_lookup(tx.key, text, &signer, &replay, NULL) == 1);
    assert(!strcmp(replay, buffer.bytes)); free(replay); free(buffer.bytes);
    srv_job_req_free(request); close(fd); close(dir); close(srv_store_lease); srv_store_lease = -1;
    return 0;
}
