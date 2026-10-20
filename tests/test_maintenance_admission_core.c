// SPDX-License-Identifier: LicenseRef-SPL-1.0
#define _GNU_SOURCE
#define JASH_AGENT_ONLY
#include "../c/core/serve.c"
#include <assert.h>

int main(int argc, char** argv) {
    assert(argc == 2 && !setenv("HOME", argv[1], 1));
    assert(!srv_store_bind() && !flock(srv_store_lease, LOCK_EX));
    srv_receipt_signer_t signer; assert(!srv_receipt_signer_load(&signer));
    const char* text = "{\"id\":\"legacy\",\"protocol_version\":2,\"op\":\"store.quota\",\"max_bytes\":123456,\"caps\":[\"store:manage\"]}";
    srv_job_req_t* request = calloc(1, sizeof(*request)); assert(request && !srv_job_parse(text, request));
    srv_job_request_digest(text, request->request_digest);
    maintenance_tx_t tx = {0}; maintenance_key(request->id_value, tx.key);
    char name[96], payload[1536]; snprintf(name, sizeof(name), "%s.intent", tx.key);
    assert(!maintenance_publish(name, text, strlen(text), 0));
    snprintf(tx.intent_ref, sizeof(tx.intent_ref), "%s.maintenance-intent.dsse", request->request_digest);
    snprintf(payload, sizeof(payload), "{\"schema\":\"jash.agent-maintenance-receipt.v1\",\"phase\":\"intent\",\"request_id\":\"legacy\",\"operation\":\"store.quota\",\"request_blake3\":\"%s\"}", request->request_digest);
    assert(!srv_typed_receipt_publish(&signer, payload, tx.intent_ref, tx.intent_hash, "agent_maintenance", maintenance_media));
    maintenance_tx_t observed; char* saved = NULL; int protocol;
    assert(!strcmp(maintenance_admission(tx.key, &signer, &observed, &saved, &protocol), "legacy") && protocol == 0); free(saved);
    char* response = NULL; int existing;
    assert(!strcmp(maintenance_candidate(tx.key, request->id_value, &signer, &response, &existing), "terminal_unavailable"));
    char outpath[4096]; snprintf(outpath, sizeof(outpath), "%s/legacy-inspection.json", argv[1]);
    int fd = open(outpath, O_WRONLY | O_CREAT | O_EXCL, 0600); assert(fd >= 0);
    strcpy(request->target_id, "legacy"); srv_maintenance_inspect(fd, request, &signer); close(fd);
    /* A genuine old signed terminal remains valid without any decision. */
    maintenance_buffer_t buffer = {0}; buffer.bytes = malloc(STORE_MAINT_MAX); assert(buffer.bytes);
    const char* body = "{\"schema\":\"jash.agent-store.v1\",\"id\":\"legacy\",\"op\":\"store.quota\",\"transaction\":\"committed\",\"ok\":true,\"applied\":true}\n";
    strcpy(buffer.bytes, body); buffer.used = strlen(body);
    unsigned char digest[32]; char hash[65], receipt_ref[224], receipt_hash[65];
    jash_blake3(body, strlen(body), digest); jash_blake3_hex(digest, hash);
    snprintf(receipt_ref, sizeof(receipt_ref), "%s.%s.maintenance-terminal.dsse", request->request_digest, hash);
    snprintf(payload, sizeof(payload), "{\"schema\":\"jash.agent-maintenance-receipt.v1\",\"phase\":\"terminal\",\"request_id\":\"legacy\",\"operation\":\"store.quota\",\"request_blake3\":\"%s\",\"response_blake3\":\"%s\",\"intent_ref\":\"%s\",\"intent_blake3\":\"%s\"}",
             request->request_digest, hash, tx.intent_ref, tx.intent_hash);
    assert(!srv_typed_receipt_publish(&signer, payload, receipt_ref, receipt_hash, "agent_maintenance", maintenance_media));
    snprintf(buffer.bytes + buffer.used - 2, STORE_MAINT_MAX - buffer.used + 2,
             ",\"receipt_phase\":\"maintenance\",\"receipt_ref\":\"%s\",\"receipt_blake3\":\"%s\",\"receipt_pubkey\":\"%s\"}\n", receipt_ref, receipt_hash, signer.pubkey_hex);
    snprintf(name, sizeof(name), "%s.result", tx.key);
    assert(!maintenance_publish(name, buffer.bytes, strlen(buffer.bytes), 0)); free(buffer.bytes);
    assert(maintenance_lookup(tx.key, text, &signer, &response, NULL) == 1); free(response);
    /* Independently signed unknown/duplicate protocol declarations fail closed. */
    char directory[896], intent_path[1400], request_path[1400]; assert(!srv_private_dir("agent_maintenance", directory, sizeof(directory)));
    snprintf(intent_path, sizeof(intent_path), "%s/%s", directory, tx.intent_ref);
    snprintf(request_path, sizeof(request_path), "%s/%s.intent", directory, tx.key);
    const char* versions[] = {"\"effect_protocol\":\"future\",", "\"effect_protocol\":\"admission-v1\",\"effect_protocol\":\"admission-v1\","};
    for (int i = 0; i < 2; i++) {
        assert(!unlink(intent_path));
        snprintf(payload, sizeof(payload), "{\"schema\":\"jash.agent-maintenance-receipt.v1\",\"phase\":\"intent\",%s\"request_id\":\"legacy\",\"operation\":\"store.quota\",\"request_blake3\":\"%s\"}", versions[i], request->request_digest);
        assert(!srv_typed_receipt_publish(&signer, payload, tx.intent_ref, tx.intent_hash, "agent_maintenance", maintenance_media));
        assert(jef_verify_maintenance_intent_protocol(intent_path, request_path, signer.pubkey_hex, &protocol) != 0);
    }
    /* A modern preparation closes once and its decision cannot become admitted. */
    text = "{\"id\":\"modern\",\"protocol_version\":2,\"op\":\"store.quota\",\"max_bytes\":123456,\"caps\":[\"store:manage\"]}";
    assert(!srv_job_parse(text, request)); srv_job_request_digest(text, request->request_digest);
    memset(&tx, 0, sizeof(tx)); maintenance_key(request->id_value, tx.key);
    snprintf(name, sizeof(name), "%s.intent", tx.key); assert(!maintenance_publish(name, text, strlen(text), 0));
    snprintf(tx.intent_ref, sizeof(tx.intent_ref), "%s.maintenance-intent.dsse", request->request_digest);
    snprintf(payload, sizeof(payload), "{\"schema\":\"jash.agent-maintenance-receipt.v1\",\"phase\":\"intent\",\"effect_protocol\":\"admission-v1\",\"request_id\":\"modern\",\"operation\":\"store.quota\",\"request_blake3\":\"%s\"}", request->request_digest);
    assert(!srv_typed_receipt_publish(&signer, payload, tx.intent_ref, tx.intent_hash, "agent_maintenance", maintenance_media));
    assert(!strcmp(maintenance_admission(tx.key, &signer, &observed, &saved, &protocol), "decision_missing") && protocol == 1);
    assert(!maintenance_close(&observed, saved, &signer)); free(saved);
    assert(maintenance_lookup(tx.key, text, &signer, &response, NULL) == 1);
    assert(strstr(response, "\"effect_admission\":\"closed\"") && strstr(response, "\"applied\":false")); free(response);
    assert(maintenance_decide(&tx, request, &signer, 1) != 0);
    srv_job_req_free(request); close(srv_store_lease); srv_store_lease = -1;
    return 0;
}
