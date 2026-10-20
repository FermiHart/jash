// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// Module: graph/jash_graph — core data structures for knowledge graph engine
// ADR-0045: Jash Graph Engine: Knowledge Graph Incremental de Código
//
// This module provides in-memory graph structures for code understanding,
// treating source code as a live graph with zero-copy parsing and incremental updates.

#ifndef JASH_GRAPH_H
#define JASH_GRAPH_H

#include <stdint.h>
#include <stddef.h>
#include <sys/stat.h>

#define JG_MAX_NODES     (1 << 20)
#define JG_MAX_EDGES     (1 << 22)
#define JG_MAX_FILES     (1 << 16)
#define JG_MAX_NAME_LEN  256
#define JG_ARENA_SIZE    (64 * 1024 * 1024)
#define JG_RING_SIZE     (1 << 16)

typedef enum {
    JG_NODE_FUNCTION,
    JG_NODE_CLASS,
    JG_NODE_MODULE,
    JG_NODE_METHOD,
    JG_NODE_VARIABLE,
    JG_NODE_STRUCT,
    JG_NODE_ENUM,
    JG_NODE_TRAIT,
    JG_NODE_INTERFACE,
    JG_NODE_MACRO,
    JG_NODE_TYPE_COUNT
} JGNodeType;

typedef enum {
    JG_REL_CALLS,
    JG_REL_IMPORTS,
    JG_REL_INHERITS,
    JG_REL_MIXES_IN,
    JG_REL_USES,
    JG_REL_CONTAINS,
    JG_REL_OVERRIDES,
    JG_REL_IMPLEMENTS,
    JG_REL_TYPE_COUNT
} JGRelType;

typedef enum { JG_CONF_EXTRACTED, JG_CONF_INFERRED } JGConfidence;

typedef struct {
    const char *name;
    const char *file;
    uint32_t line;
    uint32_t col;
    JGNodeType type;
    int32_t community;
    uint32_t file_idx;
} JGNodeData;

typedef struct {
    JGNodeData *nodes;
    uint32_t count;
    uint32_t capacity;

    uint32_t *degrees;
    uint32_t *in_degrees;
    uint32_t *out_degrees;

    const char **names;
    const char **files;
    uint32_t *lines;
    uint32_t *cols;
    JGNodeType *types;
    int32_t *communities;
    uint32_t *file_indices;
} JGNodeStore;

typedef struct {
    uint32_t src;
    uint32_t dst;
    JGRelType rel;
    JGConfidence conf;
} JGEdge;

typedef struct {
    JGEdge *data;
    uint32_t count;
    uint32_t capacity;
} JGEdgeStore;

typedef struct {
    uint32_t *row_ptr;
    uint32_t *col_idx;
    JGRelType *rel_idx;
    uint32_t size;
} JGAdjacency;

typedef struct {
    const char *path;
    uint64_t mtime;
    uint64_t size;
    int fd;
    void *map;
    size_t map_len;
} JGFileEntry;

typedef struct {
    JGFileEntry *entries;
    uint32_t count;
    uint32_t capacity;
} JGFileRegistry;

typedef struct {
    uint8_t *base;
    size_t size;
    size_t offset;
} JGArena;

typedef enum {
    JG_EVT_NODE_ADD,
    JG_EVT_NODE_REMOVE,
    JG_EVT_EDGE_ADD,
    JG_EVT_EDGE_REMOVE,
    JG_EVT_FILE_CHANGE,
    JG_EVT_REPARSE
} JGEventType;

typedef struct {
    uint64_t timestamp;
    JGEventType type;
    uint32_t node_idx;
    uint32_t edge_idx;
    uint32_t file_idx;
} JGEvent;

typedef struct {
    JGEvent *events;
    uint32_t head;
    uint32_t count;
} JGTemporalLog;

typedef struct {
    const char *key;
    uint32_t node_idx;
} JGSymbolEntry;

typedef struct {
    JGSymbolEntry *entries;
    uint32_t size;
    uint32_t used;
} JGSymbolTable;

typedef struct {
    JGNodeStore nodes;
    JGEdgeStore edges;
    JGAdjacency adj;
    JGSymbolTable symbols;
    JGFileRegistry files;
    JGTemporalLog temporal;
    JGArena arena;

    char root[1024];
    int initialized;
    int dirty;

    uint64_t parse_version;
} JGGraph;

int jg_init(JGGraph *g, const char *root);
void jg_destroy(JGGraph *g);

void *jg_arena_alloc(JGArena *a, size_t bytes);
const char *jg_intern_str(JGGraph *g, const char *s, size_t len);

uint32_t jg_add_node(JGGraph *g, const char *name, const char *file,
                     uint32_t line, uint32_t col, JGNodeType type);

uint32_t jg_add_edge(JGGraph *g, uint32_t src, uint32_t dst,
                     JGRelType rel, JGConfidence conf);

int jg_build_adjacency(JGGraph *g);
void jg_compute_degrees(JGGraph *g);

int jg_bfs_path(JGGraph *g, uint32_t src, uint32_t dst,
                uint32_t *path, uint32_t *path_len);

int jg_leiden(JGGraph *g, int iterations);

int jg_register_file(JGGraph *g, const char *path);

int jg_temporal_record(JGGraph *g, JGEventType type,
                       uint32_t node_idx, uint32_t edge_idx, uint32_t file_idx);

#endif
