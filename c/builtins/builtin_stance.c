// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — builtins/stance — operational modes.
//
//   stance list                    — list available stances
//   stance set <name>              — activate a stance
//   stance show                    — show current stance config
//
// See Docs/ADR-0035-context-engine-stances-cargo.md

#include "semantic_policy.h"
#include <string.h>
#include "builtin.h"
#include <core/stance.h>

static void stance_usage(jash_writer_t w, void* ctx) {
    jash_writes(w, ctx, "usage: stance list | set <name> | show\n\n");
    jash_writes(w, ctx, "Stances:\n");
    for (int i = 0; i < JASH_STANCE_COUNT; i++) {
        const char* name = stance_name((jash_stance_t)i);
        const char* desc = stance_description((jash_stance_t)i);
        jash_writef(w, ctx, "  %-10s  %s\n", name ? name : "?", desc ? desc : "");
    }
}

static int cmd_stance_list(jash_writer_t w, void* ctx) {
    jash_writes(w, ctx, "Available stances:\n\n");
    for (int i = 0; i < JASH_STANCE_COUNT; i++) {
        const char* name = stance_name((jash_stance_t)i);
        const char* desc = stance_description((jash_stance_t)i);
        const jash_stance_cfg_t* cfg = stance_cfg((jash_stance_t)i);

        int active = ((jash_stance_t)i == stance_get());
        jash_writef(w, ctx, "  %c %-10s  %s\n", active ? '*' : ' ',
                    name ? name : "?", desc ? desc : "");
        if (cfg) {
            jash_writef(w, ctx, "    budget=%-4llu  digest=%s\n",
                        (unsigned long long)cfg->context_budget,
                        cfg->digest_level == SESH_BRIEF ? "brief" :
                        cfg->digest_level == SESH_STANDARD ? "standard" : "deep");
        }
    }
    return 0;
}

static int cmd_stance_set(jash_writer_t w, void* ctx, const char* name) {
    if (!name || !name[0]) {
        jash_writes(w, ctx, "stance: missing stance name\n");
        return 1;
    }
    jash_stance_t id = stance_from_name(name);
    if (id >= JASH_STANCE_COUNT) {
        jash_writef(w, ctx, "stance: unknown stance '%s'\n", name);
        return 1;
    }
    if (stance_set(id) != 0) {
        jash_writes(w, ctx, "stance: failed to set stance\n");
        return 1;
    }
    jash_writef(w, ctx, "stance: set to '%s' — %s\n",
                stance_name(id), stance_description(id));
    return 0;
}

static int cmd_stance_show(jash_writer_t w, void* ctx) {
    jash_stance_t cur = stance_get();
    const jash_stance_cfg_t* cfg = stance_cfg_current();
    jash_writef(w, ctx, "Current stance: %s\n", stance_name(cur));
    jash_writef(w, ctx, "Description:    %s\n", cfg ? cfg->description : "?");
    if (cfg) {
        jash_writef(w, ctx, "Context budget: %llu tokens\n",
                    (unsigned long long)cfg->context_budget);
        jash_writef(w, ctx, "Digest level:   %s\n",
                    cfg->digest_level == SESH_BRIEF ? "brief" :
                    cfg->digest_level == SESH_STANDARD ? "standard" : "deep");
    }
    return 0;
}

int jash_b_stance(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    int structured = semantic_dispatch(s, w, ctx, "stance", "jash.stance.v1", args, semantic_stance);
    if (structured >= 0) return structured;
    (void)s;
    if (!args || !args[0]) {
        stance_usage(w, ctx);
        return 0;
    }

    /* Parse subcommand. */
    while (*args == ' ' || *args == '\t') args++;
    const char* cmd = args;

    /* Find end of subcommand. */
    const char* p = cmd;
    while (*p && *p != ' ' && *p != '\t') p++;

    if (strncmp(cmd, "list", 4) == 0 && (cmd[4] == '\0' || cmd[4] == ' ')) {
        return cmd_stance_list(w, ctx);
    }
    if (strncmp(cmd, "set", 3) == 0 && (cmd[3] == '\0' || cmd[3] == ' ')) {
        const char* name = cmd + 3;
        while (*name == ' ' || *name == '\t') name++;
        return cmd_stance_set(w, ctx, name);
    }
    if (strncmp(cmd, "show", 4) == 0 && (cmd[4] == '\0' || cmd[4] == ' ')) {
        return cmd_stance_show(w, ctx);
    }

    jash_writef(w, ctx, "stance: unknown subcommand '%.*s'\n", (int)(p - cmd), cmd);
    return 1;
}

SEMANTIC_ARGV_WRAPPER(stance, "stance", "jash.stance.v1", semantic_stance)
