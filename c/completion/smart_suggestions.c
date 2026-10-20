// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — completion/smart_suggestions — keystroke-driven learning from history.

#include "smart_suggestions.h"
#include <core/env.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include "sys/syscalls.h"

#define MAX_PATTERNS 256
#define MAX_CMD_LEN 256
#define MAX_SEQUENCE_LEN 5

typedef struct {
    char commands[MAX_SEQUENCE_LEN][MAX_CMD_LEN];
    int count;
    int score;
} pattern_t;

struct jash_ss_engine {
    jash_state_t* state;
    pattern_t patterns[MAX_PATTERNS];
    int pattern_count;
    char recent_commands[MAX_SEQUENCE_LEN][MAX_CMD_LEN];
    int recent_count;
    char persist_path[512];
};

jash_ss_engine_t* jash_ss_engine_create(jash_state_t* s) {
    jash_ss_engine_t* e = (jash_ss_engine_t*)malloc(sizeof(jash_ss_engine_t));
    if (!e) return NULL;
    e->state = s;
    e->pattern_count = 0;
    e->recent_count = 0;
    
    /* Build persist path */
    const char* home = jash_env_get(s, "HOME");
    if (!home) home = jash_tmpdir();
    snprintf(e->persist_path, sizeof(e->persist_path), "%s/.jash_suggestions", home);
    
    return e;
}

void jash_ss_engine_destroy(jash_ss_engine_t* e) {
    if (e) {
        jash_ss_engine_persist(e);
        free(e);
    }
}

void jash_ss_history_record(jash_ss_engine_t* e, const char* cmd,
                            int exit_code, uint64_t duration_ns) {
    if (!e || !cmd) return;
    (void)exit_code;
    (void)duration_ns;
    
    /* Add to recent commands */
    if (e->recent_count < MAX_SEQUENCE_LEN) {
        size_t len = strlen(cmd);
        if (len >= MAX_CMD_LEN) len = MAX_CMD_LEN - 1;
        memcpy(e->recent_commands[e->recent_count], cmd, len);
        e->recent_commands[e->recent_count][len] = '\0';
        e->recent_count++;
    } else {
        /* Shift commands */
        for (int i = 0; i < MAX_SEQUENCE_LEN - 1; i++) {
            memcpy(e->recent_commands[i], e->recent_commands[i+1], MAX_CMD_LEN);
        }
        size_t len = strlen(cmd);
        if (len >= MAX_CMD_LEN) len = MAX_CMD_LEN - 1;
        memcpy(e->recent_commands[MAX_SEQUENCE_LEN - 1], cmd, len);
        e->recent_commands[MAX_SEQUENCE_LEN - 1][len] = '\0';
    }
    
    /* Check if this completes a known pattern */
    if (e->recent_count >= 2) {
        /* Look for existing pattern */
        int found = 0;
        for (int i = 0; i < e->pattern_count; i++) {
            int match = 1;
            int seq_len = e->recent_count;
            if (e->patterns[i].count != seq_len) continue;
            
            for (int j = 0; j < seq_len; j++) {
                if (strcmp(e->patterns[i].commands[j], e->recent_commands[j]) != 0) {
                    match = 0;
                    break;
                }
            }
            
            if (match) {
                e->patterns[i].score++;
                found = 1;
                break;
            }
        }
        
        /* Add new pattern */
        if (!found && e->pattern_count < MAX_PATTERNS) {
            pattern_t* p = &e->patterns[e->pattern_count];
            p->count = e->recent_count;
            for (int j = 0; j < e->recent_count; j++) {
                memcpy(p->commands[j], e->recent_commands[j], MAX_CMD_LEN);
            }
            p->score = 1;
            e->pattern_count++;
        }
    }
}

const char* jash_ss_get_suggestion(jash_ss_engine_t* e, const char* input) {
    if (!e || !input || !*input) return NULL;
    
    /* Find best matching pattern */
    int best_score = 0;
    int best_idx = -1;
    
    for (int i = 0; i < e->pattern_count; i++) {
        /* Check if input matches start of pattern */
        if (e->patterns[i].count > 0) {
            if (strncmp(e->patterns[i].commands[0], input, strlen(input)) == 0) {
                if (e->patterns[i].score > best_score) {
                    best_score = e->patterns[i].score;
                    best_idx = i;
                }
            }
        }
    }
    
    if (best_idx >= 0 && e->patterns[best_idx].count > 1) {
        /* Return next command in sequence */
        return e->patterns[best_idx].commands[1];
    }
    
    return NULL;
}

void jash_ss_engine_persist(jash_ss_engine_t* e) {
    if (!e) return;
    
    FILE* f = fopen(e->persist_path, "w");
    if (!f) return;
    
    for (int i = 0; i < e->pattern_count; i++) {
        pattern_t* p = &e->patterns[i];
        fprintf(f, "%d", p->score);
        for (int j = 0; j < p->count; j++) {
            fprintf(f, "\t%s", p->commands[j]);
        }
        fprintf(f, "\n");
    }
    
    fclose(f);
}

void jash_ss_engine_load(jash_ss_engine_t* e) {
    if (!e) return;
    
    FILE* f = fopen(e->persist_path, "r");
    if (!f) return;
    
    char line[4096];
    while (fgets(line, sizeof(line), f) && e->pattern_count < MAX_PATTERNS) {
        /* Format: score\tcmd1\tcmd2\t... */
        int score = 0;
        char* tab = line;
        /* Parse score */
        while (*tab >= '0' && *tab <= '9') {
            score = score * 10 + (*tab - '0');
            tab++;
        }
        if (*tab != '\t') continue;
        tab++; /* Skip tab */
        
        /* Parse commands separated by tabs */
        pattern_t* p = &e->patterns[e->pattern_count];
        p->count = 0;
        p->score = score;
        
        while (*tab && *tab != '\n' && p->count < MAX_SEQUENCE_LEN) {
            const char* start = tab;
            while (*tab && *tab != '\t' && *tab != '\n') tab++;
            uint32_t cmdlen = (uint32_t)(tab - start);
            if (cmdlen >= MAX_CMD_LEN) cmdlen = MAX_CMD_LEN - 1;
            memcpy(p->commands[p->count], start, cmdlen);
            p->commands[p->count][cmdlen] = '\0';
            p->count++;
            if (*tab == '\t') tab++;
        }
        
        if (p->count > 0) e->pattern_count++;
    }
    
    fclose(f);
}
