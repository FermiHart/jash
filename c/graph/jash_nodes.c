// SPDX-License-Identifier: LicenseRef-SPL-1.0

#include "jash_graph.h"
#include <stdlib.h>
#include <string.h>

uint32_t jg_add_node(JGGraph *g, const char *name, const char *file,
                     uint32_t line, uint32_t col, JGNodeType type) {
    if (!g || !g->initialized || !name) return UINT32_MAX;

    if (g->nodes.count >= JG_MAX_NODES) return UINT32_MAX;

    uint32_t idx = g->nodes.count++;
    JGNodeData *n = &g->nodes.nodes[idx];

    n->name = name;
    n->file = file ? file : "";
    n->line = line;
    n->col = col;
    n->type = type;
    n->community = -1;
    n->file_idx = UINT32_MAX;

    if (file) {
        for (uint32_t i = 0; i < g->files.count; i++) {
            if (strcmp(g->files.entries[i].path, file) == 0) {
                n->file_idx = i;
                break;
            }
        }
    }

    return idx;
}

uint32_t jg_add_edge(JGGraph *g, uint32_t src, uint32_t dst,
                     JGRelType rel, JGConfidence conf) {
    if (!g || !g->initialized) return UINT32_MAX;
    if (src >= g->nodes.count || dst >= g->nodes.count) return UINT32_MAX;

    if (g->edges.count >= JG_MAX_EDGES) return UINT32_MAX;

    uint32_t idx = g->edges.count++;
    JGEdge *e = &g->edges.data[idx];

    e->src = src;
    e->dst = dst;
    e->rel = rel;
    e->conf = conf;

    return idx;
}

int jg_build_adjacency(JGGraph *g) {
    if (!g || !g->initialized) return -1;

    if (g->adj.row_ptr) {
        free(g->adj.row_ptr);
        g->adj.row_ptr = NULL;
    }
    if (g->adj.col_idx) {
        free(g->adj.col_idx);
        g->adj.col_idx = NULL;
    }
    if (g->adj.rel_idx) {
        free(g->adj.rel_idx);
        g->adj.rel_idx = NULL;
    }

    uint32_t node_count = g->nodes.count;
    uint32_t edge_count = g->edges.count;

    g->adj.row_ptr = calloc(node_count + 1, sizeof(uint32_t));
    if (!g->adj.row_ptr) return -1;

    g->adj.col_idx = calloc(edge_count, sizeof(uint32_t));
    if (!g->adj.col_idx) {
        free(g->adj.row_ptr);
        g->adj.row_ptr = NULL;
        return -1;
    }

    g->adj.rel_idx = calloc(edge_count, sizeof(JGRelType));
    if (!g->adj.rel_idx) {
        free(g->adj.row_ptr);
        free(g->adj.col_idx);
        g->adj.row_ptr = NULL;
        g->adj.col_idx = NULL;
        return -1;
    }
    g->adj.size = edge_count;

    for (uint32_t i = 0; i < edge_count; i++) {
        uint32_t src = g->edges.data[i].src;
        g->adj.row_ptr[src + 1]++;
    }

    for (uint32_t i = 1; i <= node_count; i++) {
        g->adj.row_ptr[i] += g->adj.row_ptr[i - 1];
    }

    uint32_t *pos = malloc(node_count * sizeof(uint32_t));
    if (!pos) {
        free(g->adj.row_ptr);
        free(g->adj.col_idx);
        free(g->adj.rel_idx);
        g->adj.row_ptr = g->adj.col_idx = g->adj.rel_idx = NULL;
        return -1;
    }

    memcpy(pos, g->adj.row_ptr, (node_count + 1) * sizeof(uint32_t));

    for (uint32_t i = 0; i < edge_count; i++) {
        uint32_t src = g->edges.data[i].src;
        uint32_t pos_src = pos[src]++;
        g->adj.col_idx[pos_src] = g->edges.data[i].dst;
        g->adj.rel_idx[pos_src] = g->edges.data[i].rel;
    }

    free(pos);
    g->dirty = 0;
    return 0;
}

void jg_compute_degrees(JGGraph *g) {
    if (!g || !g->initialized) return;

    memset(g->nodes.degrees, 0, g->nodes.count * sizeof(uint32_t));
    memset(g->nodes.in_degrees, 0, g->nodes.count * sizeof(uint32_t));
    memset(g->nodes.out_degrees, 0, g->nodes.count * sizeof(uint32_t));

    for (uint32_t i = 0; i < g->edges.count; i++) {
        uint32_t src = g->edges.data[i].src;
        uint32_t dst = g->edges.data[i].dst;
        g->nodes.out_degrees[src]++;
        g->nodes.in_degrees[dst]++;
        g->nodes.degrees[src]++;
        g->nodes.degrees[dst]++;
    }
}
