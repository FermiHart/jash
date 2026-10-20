// SPDX-License-Identifier: LicenseRef-SPL-1.0
#define _GNU_SOURCE
#define JASH_AGENT_ONLY
#include "../c/core/serve.c"
#include <assert.h>

int main(int argc, char** argv) {
    assert(argc == 2 && !setenv("HOME", argv[1], 1));
    assert(!srv_store_bind());
    srv_receipt_signer_t signer = {0}; memset(signer.pubkey_hex, '0', 64);
    maintenance_history_t first, second;
    assert(!maintenance_history_scan(&first, &signer) && !first.rows_count);
    maintenance_history_free(&first);
    char key[65], name[96]; maintenance_key("native", key);
    snprintf(name, sizeof(name), "%s.intent", key);
    const char* request = "{\"id\":\"native\",\"op\":\"store.quota\"}";
    assert(!maintenance_publish(name, request, strlen(request), 0));
    assert(!maintenance_publish("binary", "\0\xff", 2, 0));
    assert(!maintenance_history_scan(&first, &signer) && first.rows_count == 2 && first.count == 2);
    assert(!maintenance_history_scan(&second, &signer) && !strcmp(first.snapshot, second.snapshot));
    assert(!strcmp(first.rows[0].target, "native") && first.rows[0].key[0] == 't');
    assert(first.rows[1].key[0] == 'u');
    maintenance_history_free(&second);
    char cursor[96], snapshot[65]; size_t offset;
    snprintf(cursor, sizeof(cursor), "m1:%s:1", first.snapshot);
    assert(!maintenance_cursor(cursor, snapshot, &offset) && offset == 1 && !strcmp(snapshot, first.snapshot));
    assert(maintenance_cursor("m1:bad", snapshot, &offset));
    snprintf(cursor, sizeof(cursor), "m1:%s:99999999999999", first.snapshot);
    assert(maintenance_cursor(cursor, snapshot, &offset));
    assert(!maintenance_publish("binary", "\xff\0", 2, 1));
    assert(maintenance_history_stable(&first));
    assert(!maintenance_history_scan(&second, &signer) && strcmp(first.snapshot, second.snapshot));
    maintenance_history_free(&first); maintenance_history_free(&second);
    char dir[896]; assert(!srv_private_dir("agent_maintenance", dir, sizeof(dir)));
    int fd = srv_store_dir("agent_maintenance"); assert(fd >= 0);
    assert(!mkfifoat(fd, "fifo", 0600) && !symlinkat("missing", fd, "link")); close(fd);
    assert(!maintenance_history_scan(&first, &signer) && first.rows_count == 4);
    maintenance_history_free(&first);
    srv_job_req_t* query = calloc(1, sizeof(*query)); assert(query);
    query->limit = 2; strcpy(query->id_value, "page");
    char path[4096]; snprintf(path, sizeof(path), "%s/history.json", argv[1]);
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600); assert(fd >= 0);
    srv_maintenance_history(fd, query, &signer); close(fd); free(query);
    close(srv_store_lease); srv_store_lease = -1;
    return 0;
}
