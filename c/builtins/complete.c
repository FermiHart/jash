// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell
// Builtin: complete — manage completion (bash compat)

#include "builtin.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* Completion spec storage — heap-allocated on first use (ADR-0016 §5) */
#define MAX_COMPLETIONS    64
#define COMPL_CMD_LEN      64
#define COMPL_OPT_LEN     256
typedef struct {
    char command[COMPL_CMD_LEN];
    char options[COMPL_OPT_LEN];
    char type; /* 'W' = wordlist, 'A' = action */
} compl_spec_t;
static compl_spec_t* completion_specs = NULL;
static int completion_count = 0;

/* Get completion words for a command. Returns NULL if not found. */
const char* jash_complete_get_words(const char* command) {
    if (!command || !completion_specs) return NULL;
    for (int i = 0; i < completion_count; i++) {
        if (completion_specs[i].type == 'W' &&
            strcmp(completion_specs[i].command, command) == 0) {
            return completion_specs[i].options;
        }
    }
    return NULL;
}

/* Get completion action for a command. Returns NULL if not found. */
const char* jash_complete_get_action(const char* command) {
    if (!command || !completion_specs) return NULL;
    for (int i = 0; i < completion_count; i++) {
        if (completion_specs[i].type == 'A' &&
            strcmp(completion_specs[i].command, command) == 0) {
            return completion_specs[i].options;
        }
    }
    return NULL;
}

int jash_b_complete(jash_state_t* s, jash_writer_t w, void* ctx,
                    const char* args) {
    (void)s;
    if (!completion_specs) {
        completion_specs = calloc(MAX_COMPLETIONS, sizeof(compl_spec_t));
        if (!completion_specs) return 1;
    }
    if (!args) { args = ""; }

    const char* p = args;
    while (*p == ' ') p++;

    /* -W wordlist: store completion words for command */
    if (*p == '-' && *(p+1) == 'W') {
        p += 2;
        while (*p == ' ') p++;
        
        /* Parse: -W "word1 word2" command  OR  -W word1 word2 command
         * The last token is the command name, everything before is the wordlist */
        
        /* First, collect all tokens */
        char tokens[32][64];
        int ntokens = 0;
        
        while (*p && ntokens < 32) {
            while (*p == ' ') p++;
            if (!*p) break;
            
            char* tok = tokens[ntokens];
            int ti = 0;
            
            if (*p == '"' || *p == '\'') {
                /* Quoted string - treat as single token */
                char quote = *p++;
                while (*p && *p != quote && ti < 63)
                    tok[ti++] = *p++;
                if (*p == quote) p++;
            } else {
                /* Unquoted word */
                while (*p && *p != ' ' && ti < 63)
                    tok[ti++] = *p++;
            }
            tok[ti] = '\0';
            ntokens++;
        }
        
        /* Need at least 2 tokens: wordlist + command */
        if (ntokens < 2) return 0;
        
        /* Last token is command name */
        const char* cmd = tokens[ntokens - 1];
        uint32_t cmdlen = (uint32_t)strlen(cmd);
        
        /* Build wordlist from all other tokens */
        char wordlist[512];
        unsigned wi = 0;
        for (int t = 0; t < ntokens - 1; t++) {
            const char* wp = tokens[t];   /* not `w`: shadows the writer param (W6) */
            while (*wp && wi + 1 < sizeof(wordlist)) {
                wordlist[wi++] = *wp++;
            }
            if (wi + 1 < sizeof(wordlist)) wordlist[wi++] = ' ';
        }
        if (wi > 0) wi--; /* Remove trailing space */
        wordlist[wi] = '\0';
        
        if (cmdlen > 0 && cmdlen < 64 && completion_count < MAX_COMPLETIONS) {
            /* Check if command already exists */
            int found = -1;
            for (int i = 0; i < completion_count; i++) {
                if (strlen(completion_specs[i].command) == cmdlen &&
                    memcmp(completion_specs[i].command, cmd, cmdlen) == 0) {
                    found = i;
                    break;
                }
            }
            
            int idx = (found >= 0) ? found : completion_count;
            
            /* Store command name */
            for (uint32_t i = 0; i < cmdlen; i++)
                completion_specs[idx].command[i] = cmd[i];
            completion_specs[idx].command[cmdlen] = '\0';
            
            /* Store wordlist */
            strncpy(completion_specs[idx].options, wordlist, COMPL_OPT_LEN - 1);
            completion_specs[idx].options[COMPL_OPT_LEN - 1] = '\0';
            
            if (found < 0) completion_count++;
            completion_specs[idx].type = 'W';
        }
        return 0;
    }

    /* -p: print completions */
    if (*p == '-' && *(p+1) == 'p') {
        for (int i = 0; i < completion_count; i++) {
            jash_writef(w, ctx, "complete %s\n", completion_specs[i].command);
        }
        return 0;
    }

    /* -r: remove */
    if (*p == '-' && *(p+1) == 'r') {
        p += 2;
        while (*p == ' ') p++;
        const char* cmd = p;
        while (*p && *p != ' ') p++;
        uint32_t cmdlen = (uint32_t)(p - cmd);
        for (int i = 0; i < completion_count; i++) {
            if (strlen(completion_specs[i].command) == cmdlen &&
                memcmp(completion_specs[i].command, cmd, cmdlen) == 0) {
                /* Remove by shifting */
                for (int j = i; j < completion_count - 1; j++)
                    completion_specs[j] = completion_specs[j + 1];
                completion_count--;
                i--;
            }
        }
        return 0;
    }

    /* -A action: store spec */
    if (*p == '-' && *(p+1) == 'A') {
        p += 2;
        while (*p == ' ') p++;
        const char* action = p;
        while (*p && *p != ' ') p++;
        while (*p == ' ') p++;
        const char* cmd = p;
        while (*p && *p != ' ') p++;
        uint32_t cmdlen = (uint32_t)(p - cmd);

        if (cmdlen > 0 && cmdlen < 64 && completion_count < MAX_COMPLETIONS) {
            for (uint32_t i = 0; i < cmdlen; i++)
                completion_specs[completion_count].command[i] = cmd[i];
            completion_specs[completion_count].command[cmdlen] = '\0';
            /* Store action in options */
            const char* a = action;
            uint32_t ai = 0;
            while (*a && *a != ' ' && ai < COMPL_OPT_LEN - 1)
                completion_specs[completion_count].options[ai++] = *a++;
            completion_specs[completion_count].options[ai] = '\0';
            completion_specs[completion_count].type = 'A';
            completion_count++;
        }
        return 0;
    }

    /* Default: no args or unrecognized — just return success */
    return 0;
}
