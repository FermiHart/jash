// SPDX-License-Identifier: LicenseRef-SPL-1.0
// Builtin: abbr — abbreviations with local/global/command-scoped.

#include "builtin.h"
#include <core/env.h>
#include <string.h>

#define JASH_ABBR_CMD_MAX  32
#define JASH_ABBR_CMD_KEY_LEN  64
#define JASH_ABBR_CMD_VAL_LEN 256

typedef struct {
    char cmd[JASH_ABBR_CMD_KEY_LEN];
    char key[JASH_ABBR_CMD_KEY_LEN];
    char val[JASH_ABBR_CMD_VAL_LEN];
} jash_abbr_cmd_entry_t;

static jash_abbr_cmd_entry_t g_cmd_abbr[64];
static uint32_t g_cmd_abbr_count = 0;

static const char* skip_ws(const char* p) { while (*p == ' ' || *p == '\t') p++; return p; }

static int abbr_list_has(const jash_abbr_entry_t* tbl, uint32_t count,
                          const char* key) {
    for (uint32_t i = 0; i < count; i++) {
        if (strcmp(tbl[i].key, key) == 0) return (int)i;
    }
    return -1;
}

static int abbr_set_in_table(jash_abbr_entry_t* tbl, uint32_t* count,
                              uint32_t max, const char* key, const char* val) {
    int idx = abbr_list_has(tbl, *count, key);
    if (idx >= 0) {
        strncpy(tbl[idx].val, val, JASH_ABBR_VAL_LEN - 1);
        tbl[idx].val[JASH_ABBR_VAL_LEN - 1] = '\0';
        return 0;
    }
    if (*count >= max) return -1;
    strncpy(tbl[*count].key, key, JASH_ABBR_KEY_LEN - 1);
    tbl[*count].key[JASH_ABBR_KEY_LEN - 1] = '\0';
    strncpy(tbl[*count].val, val, JASH_ABBR_VAL_LEN - 1);
    tbl[*count].val[JASH_ABBR_VAL_LEN - 1] = '\0';
    (*count)++;
    return 0;
}

static int abbr_unset_in_table(jash_abbr_entry_t* tbl, uint32_t* count,
                                const char* key) {
    int idx = abbr_list_has(tbl, *count, key);
    if (idx < 0) return -1;
    (*count)--;
    if ((uint32_t)idx < *count) {
        tbl[idx] = tbl[*count];
    }
    return 0;
}

/* Command-scoped abbreviation lookup */
static const char* abbr_cmd_lookup(const char* cmd, const char* name) {
    for (uint32_t i = 0; i < g_cmd_abbr_count; i++) {
        if (strcmp(g_cmd_abbr[i].cmd, cmd) == 0 &&
            strcmp(g_cmd_abbr[i].key, name) == 0)
            return g_cmd_abbr[i].val;
    }
    return 0;
}

static int abbr_cmd_set(const char* cmd, const char* key, const char* val) {
    /* Update existing */
    for (uint32_t i = 0; i < g_cmd_abbr_count; i++) {
        if (strcmp(g_cmd_abbr[i].cmd, cmd) == 0 &&
            strcmp(g_cmd_abbr[i].key, key) == 0) {
            strncpy(g_cmd_abbr[i].val, val, JASH_ABBR_CMD_VAL_LEN - 1);
            return 0;
        }
    }
    if (g_cmd_abbr_count >= JASH_ABBR_CMD_MAX) return -1;
    strncpy(g_cmd_abbr[g_cmd_abbr_count].cmd, cmd, JASH_ABBR_CMD_KEY_LEN - 1);
    strncpy(g_cmd_abbr[g_cmd_abbr_count].key, key, JASH_ABBR_CMD_KEY_LEN - 1);
    strncpy(g_cmd_abbr[g_cmd_abbr_count].val, val, JASH_ABBR_CMD_VAL_LEN - 1);
    g_cmd_abbr_count++;
    return 0;
}

int jash_b_abbr(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    const char* p = skip_ws(args ? args : "");
    if (!*p) {
        /* List all */
        for (uint32_t i = 0; i < s->abbr_global_count; i++)
            jash_writef(w, ctx, "abbr %s %s\n",
                        s->abbr_global[i].key, s->abbr_global[i].val);
        for (uint32_t i = 0; i < s->abbr_local_count; i++)
            jash_writef(w, ctx, "abbr --local %s %s\n",
                        s->abbr_local[i].key, s->abbr_local[i].val);
        for (uint32_t i = 0; i < g_cmd_abbr_count; i++)
            jash_writef(w, ctx, "abbr --command %s %s %s\n",
                        g_cmd_abbr[i].cmd, g_cmd_abbr[i].key, g_cmd_abbr[i].val);
        return 0;
    }
    if (strncmp(p, "-e ", 3) == 0 || strncmp(p, "--erase ", 8) == 0) {
        p = skip_ws(p[1] == 'e' ? p + 3 : p + 8);
        char key[JASH_ABBR_KEY_LEN];
        unsigned ki = 0;
        while (*p && *p != ' ' && *p != '\t' && ki + 1 < sizeof(key))
            key[ki++] = *p++;
        key[ki] = 0;
        if (!key[0]) {
            jash_writes(w, ctx, "usage: abbr -e NAME\n");
            return 2;
        }
        abbr_unset_in_table(s->abbr_local, &s->abbr_local_count, key);
        abbr_unset_in_table(s->abbr_global, &s->abbr_global_count, key);
        return 0;
    }

    int is_local = 0;
    int is_global = 0;
    const char* cmd_scope = 0;

    if (strncmp(p, "--local ", 8) == 0) { is_local = 1; p += 8; }
    else if (strncmp(p, "--global ", 9) == 0) { is_global = 1; p += 9; }
    else if (strncmp(p, "--command ", 10) == 0) {
        p += 10;
        /* Parse command name */
        static char cmd_buf[JASH_ABBR_CMD_KEY_LEN];
        unsigned ci = 0;
        while (*p && *p != ' ' && *p != '\t' && ci + 1 < sizeof(cmd_buf))
            cmd_buf[ci++] = *p++;
        cmd_buf[ci] = 0;
        cmd_scope = cmd_buf;
        p = skip_ws(p);
    }

    p = skip_ws(p);
    if (!*p) {
        if (is_local || is_global) {
            const jash_abbr_entry_t* tbl = is_local ? s->abbr_local : s->abbr_global;
            uint32_t cnt = is_local ? s->abbr_local_count : s->abbr_global_count;
            for (uint32_t i = 0; i < cnt; i++)
                jash_writef(w, ctx, "abbr %s %s %s\n",
                            is_local ? "--local" : "--global",
                            tbl[i].key, tbl[i].val);
            return 0;
        }
        jash_writes(w, ctx, "usage: abbr [--global|--local|--command CMD] NAME EXPANSION\n");
        return 2;
    }

    char name[JASH_ABBR_KEY_LEN];
    unsigned ni = 0;
    while (*p && *p != ' ' && *p != '\t' && ni + 1 < sizeof(name))
        name[ni++] = *p++;
    name[ni] = 0;
    p = skip_ws(p);
    if (!name[0] || !*p) {
        jash_writes(w, ctx, "usage: abbr [--global|--local|--command CMD] NAME EXPANSION\n");
        return 2;
    }

    /* Command-scoped abbreviation */
    if (cmd_scope) {
        abbr_cmd_set(cmd_scope, name, p);
        return 0;
    }

    if (!is_local && !is_global) is_global = 1;

    if (is_global) {
        abbr_set_in_table(s->abbr_global, &s->abbr_global_count,
                          JASH_ABBR_GLOBAL_MAX, name, p);
    }
    if (is_local) {
        abbr_set_in_table(s->abbr_local, &s->abbr_local_count,
                          JASH_ABBR_LOCAL_MAX, name, p);
    }
    return 0;
}

/* Export for jux.c to use in command-scoped expansion */
__attribute__((unused)) static const char* jash_abbr_cmd_lookup(const char* cmd, const char* name) {
    return abbr_cmd_lookup(cmd, name);
}
