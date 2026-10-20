// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// Module: graph/jash_arena — bump allocator, zero free, zero fragmentation

#include "jash_graph.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/mman.h>

int jg_init(JGGraph *g, const char *root) {
    if (!g) return -1;
    memset(g, 0, sizeof(JGGraph));

    g->arena.base = malloc(JG_ARENA_SIZE);
    if (!g->arena.base) return -1;
    g->arena.size = JG_ARENA_SIZE;
    g->arena.offset = 0;

    g->nodes.capacity = JG_MAX_NODES;
    g->nodes.nodes = calloc(JG_MAX_NODES, sizeof(JGNodeData));
    g->nodes.degrees = calloc(JG_MAX_NODES, sizeof(uint32_t));
    g->nodes.in_degrees = calloc(JG_MAX_NODES, sizeof(uint32_t));
    g->nodes.out_degrees = calloc(JG_MAX_NODES, sizeof(uint32_t));
    if (!g->nodes.nodes || !g->nodes.degrees || 
        !g->nodes.in_degrees || !g->nodes.out_degrees) {
        jg_destroy(g);
        return -1;
    }
    g->nodes.count = 0;

    g->edges.capacity = JG_MAX_EDGES;
    g->edges.data = calloc(JG_MAX_EDGES, sizeof(JGEdge));
    if (!g->edges.data) {
        jg_destroy(g);
        return -1;
    }
    g->edges.count = 0;

    g->files.capacity = JG_MAX_FILES;
    g->files.entries = calloc(JG_MAX_FILES, sizeof(JGFileEntry));
    if (!g->files.entries) {
        jg_destroy(g);
        return -1;
    }
    g->files.count = 0;

    g->temporal.events = calloc(JG_RING_SIZE, sizeof(JGEvent));
    if (!g->temporal.events) {
        jg_destroy(g);
        return -1;
    }
    g->temporal.head = 0;
    g->temporal.count = 0;

    if (root) {
        strlcpy(g->root, root, sizeof(g->root));
        g->root[sizeof(g->root) - 1] = '\0';
    }

    g->initialized = 1;
    g->dirty = 0;
    g->parse_version = 0;

    return 0;
}

void jg_destroy(JGGraph *g) {
    if (!g) return;

    if (g->arena.base) {
        free(g->arena.base);
        g->arena.base = NULL;
    }

    if (g->adj.row_ptr) free(g->adj.row_ptr);
    if (g->adj.col_idx) free(g->adj.col_idx);
    if (g->adj.rel_idx) free(g->adj.rel_idx);

    if (g->nodes.nodes) free(g->nodes.nodes);
    if (g->nodes.degrees) free(g->nodes.degrees);
    if (g->nodes.in_degrees) free(g->nodes.in_degrees);
    if (g->nodes.out_degrees) free(g->nodes.out_degrees);

    if (g->edges.data) free(g->edges.data);

    if (g->files.entries) {
        for (uint32_t i = 0; i < g->files.count; i++) {
            if (g->files.entries[i].fd > 0) close(g->files.entries[i].fd);
            if (g->files.entries[i].map)munmap(g->files.entries[i].map, g->files.entries[i].map_len);
        }
        free(g->files.entries);
    }

    if (g->temporal.events) free(g->temporal.events);
    if (g->symbols.entries) free(g->symbols.entries);

    memset(g, 0, sizeof(JGGraph));
}

void *jg_arena_alloc(JGArena *a, size_t bytes) {
    if (!a || !a->base) return NULL;

    size_t aligned = (bytes + 15) & ~((size_t)15);
    if (a->offset + aligned > a->size) {
        errno = ENOMEM;
        return NULL;
    }

    void *ptr = (char*)a->base + a->offset;
    a->offset += aligned;
    return ptr;
}

const char *jg_intern_str(JGGraph *g, const char *s, size_t len) {
    if (!g || !s || len == 0) return "";

    char *copy = jg_arena_alloc(&g->arena, len + 1);
    if (!copy) return s;

    memcpy(copy, s, len);
    copy[len] = '\0';
    return copy;
}
