// SPDX-License-Identifier: LicenseRef-SPL-1.0
#define _GNU_SOURCE
#include "core/ai_observation.h"
#include "osif/buffer.h"
#include "osif/ipc.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "observation check failed at %d: %s (errno %d)\n", \
            __LINE__, #condition, errno); return 1; \
} } while (0)

int main(void) {
    char catalog[1024];
    CHECK(jash_ai_observation_describe(catalog, sizeof(catalog)) == 0);
    CHECK(strstr(catalog, "\"name\":\"fs.list\"") != NULL);
    CHECK(strstr(catalog, "\"name\":\"fs.observation.read\"") != NULL);
    CHECK(strstr(catalog, "\"name\":\"fs.source.read\"") != NULL);
    CHECK(strstr(catalog, "\"effect\":\"read\"") != NULL);
    char catalog_short[4] = "old";
    CHECK(jash_ai_observation_describe(catalog_short,
                                       sizeof(catalog_short)) < 0);
    CHECK(catalog_short[0] == '\0');
    char root[] = "/tmp/jash-ai-observation-XXXXXX";
    CHECK(mkdtemp(root) != NULL);
    char path[512];
    CHECK(snprintf(path, sizeof(path), "%s/a", root) > 0);
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    CHECK(fd >= 0 && close(fd) == 0);
    struct stat before, after;
    CHECK(stat(path, &before) == 0);
    char observation[4096];
    CHECK(jash_ai_observe_ls(root, observation, sizeof(observation)) == 0);
    CHECK(strstr(observation, "\"operation\":\"fs.list\"") != NULL);
    CHECK(strstr(observation, "\"name\":\"a\"") != NULL);
    CHECK(strstr(observation, "\"complete\":true") != NULL);
    CHECK(strstr(observation, "\"items_seen\":1") != NULL);
    const char* ref_start = strstr(observation, "\"observation_ref\":\"");
    CHECK(ref_start != NULL);
    ref_start += strlen("\"observation_ref\":\"");
    const char* ref_end = strchr(ref_start, '"');
    CHECK(ref_end != NULL && (size_t)(ref_end - ref_start) < 160);
    char ref[160];
    memcpy(ref, ref_start, (size_t)(ref_end - ref_start));
    ref[ref_end - ref_start] = '\0';
    char page[2048];
    CHECK(jash_ai_observation_read_json(ref, 0, 64, page, sizeof(page)) == 0);
    CHECK(strstr(page, "\"encoding\":\"base64\"") != NULL);
    CHECK(strstr(page, "\"offset\":0") != NULL);
    CHECK(strstr(page, "\"total_bytes\":") != NULL);
    char replay_page[2048];
    CHECK(jash_ai_observation_read_json(ref, 0, 64,
                                        replay_page, sizeof(replay_page)) == 0);
    CHECK(strcmp(replay_page, page) == 0);
    const char* total_text = strstr(page, "\"total_bytes\":");
    CHECK(total_text != NULL);
    unsigned long long total = 0;
    CHECK(sscanf(total_text, "\"total_bytes\":%llu", &total) == 1);
    CHECK(total >= 64 && total < SIZE_MAX);
    uint64_t exported_size = 0;
    int reader = jash_ai_observation_reader_fd(ref, &exported_size);
    CHECK(reader >= 0 && exported_size == total);
    CHECK((fcntl(reader, F_GETFL) & O_ACCMODE) == O_RDONLY);
    char magic[4];
    CHECK(read(reader, magic, sizeof(magic)) == sizeof(magic));
    CHECK(memcmp(magic, "JTV1", sizeof(magic)) == 0);
    int independent = jash_ai_observation_reader_fd(ref, &exported_size);
    CHECK(independent >= 0);
    CHECK(read(independent, magic, sizeof(magic)) == sizeof(magic));
    CHECK(memcmp(magic, "JTV1", sizeof(magic)) == 0);
    CHECK(lseek(reader, 0, SEEK_CUR) == (off_t)sizeof(magic));
    int lane[2];
    CHECK(jash_ipc_data_pair(lane) == 0);
    CHECK(jash_ipc_data_send(lane[0], "observation-1", exported_size,
                              reader) == 0);
    int transferred = -1;
    uint64_t transferred_size = 0;
    CHECK(jash_ipc_data_receive(lane[1], "observation-1",
                                 &transferred_size, &transferred) == 0);
    CHECK(transferred_size == exported_size && transferred >= 0);
    CHECK(jash_buffer_validate(transferred, transferred_size,
                                8u * 1024u * 1024u) == 0);
    CHECK(close(transferred) == 0);
    CHECK(close(lane[0]) == 0 && close(lane[1]) == 0);
    CHECK(close(reader) == 0 && close(independent) == 0);
    CHECK(jash_ai_observation_read_json(ref, (size_t)total - 16u, 64,
                                        replay_page, sizeof(replay_page)) == 0);
    CHECK(strstr(replay_page, "\"bytes_returned\":16") != NULL);
    CHECK(strstr(replay_page, "\"eof\":true") != NULL);
    CHECK(jash_ai_observation_read_json(ref, (size_t)total + 1u, 64,
                                        replay_page, sizeof(replay_page)) < 0);
    CHECK(errno == ERANGE && replay_page[0] == '\0');
    char page_tiny[8] = "dirty";
    CHECK(jash_ai_observation_read_json(ref, 0, 64,
                                        page_tiny, sizeof(page_tiny)) < 0);
    CHECK(page_tiny[0] == '\0');
    CHECK(jash_ai_observation_read_json("o:unknown", 0, 64,
                                        page, sizeof(page)) < 0);
    CHECK(page[0] == '\0');
    CHECK(stat(path, &after) == 0);
    CHECK(before.st_ino == after.st_ino && before.st_size == after.st_size &&
          before.st_mtime == after.st_mtime && before.st_ctime == after.st_ctime);

    unsigned char raw_name[] = {0xff, '-', 'n', 0};
    CHECK(snprintf(path, sizeof(path), "%s/", root) > 0);
    size_t prefix = strlen(path);
    memcpy(path + prefix, raw_name, sizeof(raw_name));
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    CHECK(fd >= 0 && close(fd) == 0);
    CHECK(jash_ai_observation_read_json(ref, 0, 64, page, sizeof(page)) < 0);
    CHECK(errno == ESTALE && page[0] == '\0');
    CHECK(jash_ai_observation_reader_fd(ref, &exported_size) < 0);
    CHECK(errno == ESTALE);
    CHECK(jash_ai_observe_ls(root, observation, sizeof(observation)) == 0);
    CHECK(strstr(observation, "\"encoding\":\"base64\"") != NULL);
    CHECK(jash_ai_observation_read_json(ref, 0, 64, page, sizeof(page)) < 0);
    CHECK(page[0] == '\0');

    for (int i = 0; i < 12; i++) {
        CHECK(snprintf(path, sizeof(path), "%s/entry-%02d", root, i) > 0);
        fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
        CHECK(fd >= 0 && close(fd) == 0);
    }
    CHECK(jash_ai_observe_ls(root, observation, sizeof(observation)) == 0);
    CHECK(strstr(observation, "\"items_seen\":14") != NULL);
    CHECK(strstr(observation, "\"projection_truncated\":true") != NULL);
    CHECK(strstr(observation, "\"complete\":true") != NULL);

    char small[8] = "dirty";
    CHECK(jash_ai_observe_ls(root, small, sizeof(small)) < 0);
    CHECK(small[0] == '\0');
    CHECK(jash_ai_observation_read_json("latest", 0, 64,
                                        page, sizeof(page)) == 0);
    for (int i = 0; i < 12; i++) {
        CHECK(snprintf(path, sizeof(path), "%s/entry-%02d", root, i) > 0);
        CHECK(unlink(path) == 0);
    }
    CHECK(snprintf(path, sizeof(path), "%s/", root) > 0);
    prefix = strlen(path);
    memcpy(path + prefix, raw_name, sizeof(raw_name));
    CHECK(unlink(path) == 0);
    CHECK(snprintf(path, sizeof(path), "%s/a", root) > 0);
    CHECK(unlink(path) == 0 && rmdir(root) == 0);
    jash_ai_observation_forget();
    CHECK(jash_ai_observation_read_json("latest", 0, 64,
                                        page, sizeof(page)) < 0);
    CHECK(errno == ENOENT && page[0] == '\0');
    puts("ai typed observation: ok");
    return 0;
}
