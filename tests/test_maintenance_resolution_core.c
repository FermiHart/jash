// SPDX-License-Identifier: LicenseRef-SPL-1.0
#define _GNU_SOURCE
#define JASH_AGENT_ONLY
#include "../c/core/serve.c"
#include <assert.h>

int main(int argc, char** argv) {
    assert(argc == 4 && !setenv("HOME", argv[1], 1));
    assert(!srv_store_bind() && !flock(srv_store_lease, LOCK_EX));
    srv_receipt_signer_t signer = {0};
    assert(strlen(argv[3]) == 64); memcpy(signer.pubkey_hex, argv[3], 65);
    char key[65], name[96], *result = NULL; int existing;
    maintenance_key(argv[2], key);
    assert(!maintenance_candidate(key, argv[2], &signer, &result, &existing));
    assert(result && !existing);
    char* expected = strdup(result); assert(expected); free(result);
    int directory = srv_store_dir("agent_maintenance"); assert(directory >= 0);
    snprintf(name, sizeof(name), ".%s.result.0.0.tmp", key);
    int fd = openat(directory, name, O_WRONLY | O_CREAT | O_EXCL, 0600); assert(fd >= 0);
    close(fd);
    const char* error = maintenance_candidate(key, argv[2], &signer, &result, &existing);
    assert(error && !strcmp(error, "candidate_invalid") && !result);
    fd = openat(directory, name, O_WRONLY | O_NOFOLLOW); assert(fd >= 0);
    assert(!srv_write_all(fd, expected, strlen(expected)) && !fsync(fd)); close(fd);
    assert(!maintenance_candidate(key, argv[2], &signer, &result, &existing));
    assert(!existing && !strcmp(result, expected)); free(result);
    snprintf(name, sizeof(name), "%s.result", key);
    assert(!maintenance_publish(name, expected, strlen(expected), 0));
    assert(!maintenance_candidate(key, argv[2], &signer, &result, &existing));
    assert(existing && !strcmp(result, expected) && !maintenance_sync_result(name));
    srv_job_req_t* query = calloc(1, sizeof(*query)); assert(query);
    assert(strlen(argv[2]) < sizeof(query->target_id)); strcpy(query->target_id, argv[2]);
    strcpy(query->id_value, "native-inspection");
    char output_path[4096]; snprintf(output_path, sizeof(output_path), "%s/inspection.json", argv[1]);
    int output = open(output_path, O_RDWR | O_CREAT | O_EXCL, 0600); assert(output >= 0);
    srv_maintenance_inspect(output, query, &signer);
    assert(lseek(output, 0, SEEK_SET) == 0);
    char report[8192]; ssize_t length = read(output, report, sizeof(report) - 1);
    assert(length > 0); report[length] = 0;
    assert(strstr(report, "\"state\":\"committed\"") && strstr(report, "\"receipt_status\":\"verified\""));
    assert(strstr(report, "\"resolution_eligible\":true") && strstr(report, "\"scan_complete\":true"));
    close(output); free(query);
    free(result); free(expected); close(directory);
    close(srv_store_lease); srv_store_lease = -1;
    return 0;
}
