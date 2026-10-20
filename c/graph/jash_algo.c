// SPDX-License-Identifier: LicenseRef-SPL-1.0

#include "jash_graph.h"
#include <stdlib.h>
#include <string.h>

int jg_bfs_path(JGGraph *g, uint32_t src, uint32_t dst,
                uint32_t *path, uint32_t *path_len) {
    if (!g || !g->initialized || !path || !path_len) return -1;
    if (src >= g->nodes.count || dst >= g->nodes.count) return -1;

    if (g->dirty && jg_build_adjacency(g) != 0) return -1;

    uint32_t *parent = calloc(g->nodes.count, sizeof(uint32_t));
    if (!parent) return -1;

    uint32_t *queue = calloc(g->nodes.count, sizeof(uint32_t));
    if (!queue) {
        free(parent);
        return -1;
    }

    memset(parent, 0xFF, g->nodes.count * sizeof(uint32_t));

    uint32_t head = 0, tail = 0;
    queue[tail++] = src;
    parent[src] = src;

    while (head < tail && parent[dst] == UINT32_MAX) {
        uint32_t cur = queue[head++];

        for (uint32_t e = g->adj.row_ptr[cur];
             e < g->adj.row_ptr[cur + 1]; e++) {
            uint32_t next = g->adj.col_idx[e];
            if (parent[next] == UINT32_MAX) {
                parent[next] = cur;
                queue[tail++] = next;
            }
        }
    }

    int found = (parent[dst] != UINT32_MAX);

    if (found) {
        uint32_t len = 0;
        uint32_t cur = dst;
        while (cur != src) {
            path[len++] = cur;
            cur = parent[cur];
        }
        path[len++] = src;

        for (uint32_t i = 0; i < len / 2; i++) {
            uint32_t tmp = path[i];
            path[i] = path[len - 1 - i];
            path[len - 1 - i] = tmp;
        }

        *path_len = len;
    } else {
        *path_len = 0;
    }

    free(parent);
    free(queue);
    return found ? 0 : 1;
}

static int uf_find(uint32_t *parent, uint32_t *rank, uint32_t x) {
    while (parent[x] != x) {
        parent[x] = parent[parent[x]];
        x = parent[x];
    }
    return x;
}

static void uf_union(uint32_t *parent, uint32_t *rank, uint32_t a, uint32_t b) {
    a = uf_find(parent, rank, a);
    b = uf_find(parent, rank, b);
    if (a == b) return;
    if (rank[a] < rank[b]) {
        uint32_t t = a;
        a = b;
        b = t;
    }
    parent[b] = a;
    if (rank[a] == rank[b]) rank[a]++;
}

int jg_leiden(JGGraph *g, int iterations) {
    if (!g || !g->initialized) return -1;
    if (g->nodes.count == 0) return 0;

    if (g->dirty && jg_build_adjacency(g) != 0) return -1;

    uint32_t *parent = calloc(g->nodes.count, sizeof(uint32_t));
    uint32_t *rank = calloc(g->nodes.count, sizeof(uint32_t));
    if (!parent || !rank) {
        free(parent);
        free(rank);
        return -1;
    }

    for (uint32_t i = 0; i < g->nodes.count; i++) {
        parent[i] = i;
        rank[i] = 0;
        g->nodes.nodes[i].community = (int32_t)i;
    }

    int *changed = calloc(g->nodes.count, sizeof(int));
    if (!changed) {
        free(parent);
        free(rank);
        return -1;
    }

    for (int iter = 0; iter < iterations && iter < 100; iter++) {
        int total_changed = 0;
        memset(changed, 0, g->nodes.count * sizeof(int));

        for (uint32_t i = 0; i < g->nodes.count; i++) {
            int best_community = g->nodes.nodes[i].community;
            int best_score = 0;

            int neighbor_count[JG_MAX_NODES];
            int neighbor_community[JG_MAX_NODES];
            int n = 0;

            for (uint32_t e = g->adj.row_ptr[i]; e < g->adj.row_ptr[i + 1]; e++) {
                uint32_t neighbor = g->adj.col_idx[e];
                int c = g->nodes.nodes[neighbor].community;

                int found = 0;
                for (int j = 0; j < n; j++) {
                    if (neighbor_community[j] == c) {
                        neighbor_count[j]++;
                        found = 1;
                        break;
                    }
                }
                if (!found && n < JG_MAX_NODES) {
                    neighbor_count[n] = 1;
                    neighbor_community[n] = c;
                    n++;
                }
            }

            for (int j = 0; j < n; j++) {
                int score = neighbor_count[j] - (best_community == neighbor_community[j] ? 
                                                (int)g->nodes.degrees[i] : 0);
                if (score > best_score) {
                    best_score = score;
                    best_community = neighbor_community[j];
                }
            }

            if (best_community != g->nodes.nodes[i].community) {
                g->nodes.nodes[i].community = best_community;
                changed[i] = 1;
                total_changed++;
            }
        }

        for (uint32_t i = 0; i < g->nodes.count; i++) {
            if (changed[i]) {
                uint32_t root = uf_find(parent, rank, i);
                uint32_t root_best = uf_find(parent, rank, g->nodes.nodes[i].community);
                if (root != root_best) {
                    uf_union(parent, rank, root, root_best);
                }
            }
        }

        if (total_changed == 0) break;
    }

    for (uint32_t i = 0; i < g->nodes.count; i++) {
        g->nodes.nodes[i].community = (int32_t)uf_find(parent, rank, i);
    }

    free(parent);
    free(rank);
    free(changed);

    return 0;
}
