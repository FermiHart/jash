// SPDX-License-Identifier: LicenseRef-SPL-1.0

#include "jash_graph.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>

int jg_register_file(JGGraph *g, const char *path) {
    if (!g || !g->initialized || !path) return -1;

    struct stat st;
    if (stat(path, &st) != 0) return -1;

    for (uint32_t i = 0; i < g->files.count; i++) {
        if (strcmp(g->files.entries[i].path, path) == 0) {
            return (int)i;
        }
    }

    if (g->files.count >= JG_MAX_FILES) return -1;

    JGFileEntry *entry = &g->files.entries[g->files.count];
    entry->path = strdup(path);
    entry->mtime = st.st_mtime;
    entry->size = st.st_size;
    entry->fd = -1;
    entry->map = NULL;
    entry->map_len = 0;

    if (g->parse_version > 0) {
        g->parse_version++;
    }

    return (int)g->files.count++;
}

int jg_temporal_record(JGGraph *g, JGEventType type,
                         uint32_t node_idx, uint32_t edge_idx, uint32_t file_idx) {
    if (!g || !g->initialized) return -1;

    JGEvent *evt = &g->temporal.events[g->temporal.head];
    evt->timestamp = (uint64_t)time(NULL) * 1000;
    evt->type = type;
    evt->node_idx = node_idx;
    evt->edge_idx = edge_idx;
    evt->file_idx = file_idx;

    g->temporal.head = (g->temporal.head + 1) % JG_RING_SIZE;
    if (g->temporal.count < JG_RING_SIZE) g->temporal.count++;

    return 0;
}
