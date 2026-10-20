// SPDX-License-Identifier: LicenseRef-SPL-1.0
#define _GNU_SOURCE
#define JASH_AGENT_ONLY
#include "../c/core/serve.c"
#include <assert.h>

int main(int argc, char** argv) {
    assert(argc == 2 && !chdir(argv[1]) && !setenv("HOME", argv[1], 1));
    assert(!srv_store_bind() && !flock(srv_store_lease, LOCK_EX));
    int source = open("source", O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(source >= 0 && write(source, "data", 4) == 4); close(source);
    unsigned char digest[32]; char hash[65], ref[160];
    jash_blake3("data", 4, digest); jash_blake3_hex(digest, hash);
    assert(!srv_cas_publish("source", 4, hash, ref, sizeof(ref)));
    char path[256]; snprintf(path, sizeof(path), ".jash/overflow/%s", ref);
    int policy = open(".jash/agent_store/quota", O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(policy >= 0 && write(policy, "4\n", 2) == 2); close(policy);
    int admission;
    /* A stale pre-lock observation must not charge a concurrent dedup winner twice. */
    assert(!srv_store_admit(4, path, &admission));
    if (admission >= 0) close(admission);
    srv_receipt_signer_t signer = {0}; store_catalog_t catalog;
    store_catalog_scan(&catalog, &signer); assert(catalog.complete && catalog.count == 1);
    srv_job_req_t* request = calloc(1, sizeof(*request)); assert(request); request->dry_run = 1;
    int output = open("/dev/null", O_WRONLY); assert(output >= 0);
    jash_json_writer_t writer; jash_jsonw_init(&writer, jash_jsonw_fd_sink, &output);
    assert(!store_gc(&catalog, request, &writer) && catalog.planned == 1 && !access(path, F_OK));
    request->dry_run = 0;
    assert(!store_gc(&catalog, request, &writer) && catalog.reclaimed == 1 && access(path, F_OK));
    store_catalog_free(&catalog);
    assert(!srv_cas_publish("source", 4, hash, ref, sizeof(ref)));
    char directory[896]; assert(!srv_private_dir("agent_requests", directory, sizeof(directory)));
    int malformed = open(".jash/agent_requests/bad.req", O_WRONLY | O_CREAT | O_EXCL, 0600);
    char root[68]; memset(root, 'a', 64); memcpy(root + 64, "\nx\n", 3);
    assert(malformed >= 0 && write(malformed, root, 67) == 67); close(malformed);
    store_catalog_scan(&catalog, &signer); assert(!catalog.complete);
    assert(store_gc(&catalog, request, &writer) != 0 && !access(path, F_OK));
    store_catalog_free(&catalog);
    char invalid[256]; memset(invalid, '0', sizeof(invalid)); memcpy(invalid, "blake3-", 7); invalid[71] = '-';
    memcpy(invalid + sizeof(invalid) - 5, ".bin", 5); uint64_t size;
    assert(!store_cas_name(invalid, hash, &size));
    free(request); close(output); close(srv_store_lease); srv_store_lease = -1;
    return 0;
}
