// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// Builtin: getopts — parse positional parameters

#include "builtin.h"
#include <core/state.h>
#include <core/env.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char* skip_ws(const char* p) {
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

int jash_b_getopts(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {

    // Parse: getopts optstring name [arg]
    const char* p = args;
    
    // Skip leading spaces
    p = skip_ws(p);
    
    if (!*p) {
        jash_writef(w, ctx, "getopts: usage: getopts optstring name [arg]\n");
        return 2;
    }
    
    const char* optstring_start = p;
    while (*p && *p != ' ' && *p != '\t') p++;
    
    size_t optstring_len = p - optstring_start;
    if (optstring_len == 0) {
        jash_writes(w, ctx, "getopts: optstring must not be empty\n");
        return 2;
    }
    
    char optstring[128];
    if (optstring_len >= sizeof(optstring)) optstring_len = sizeof(optstring) - 1;
    for (size_t i = 0; i < optstring_len; i++) optstring[i] = optstring_start[i];
    optstring[optstring_len] = '\0';
    
    p = skip_ws(p);
    if (!*p) {
        jash_writes(w, ctx, "getopts: name argument required\n");
        return 2;
    }
    
    const char* name_start = p;
    while (*p && *p != ' ' && *p != '\t') p++;
    
    size_t name_len = p - name_start;
    if (name_len == 0) {
        jash_writes(w, ctx, "getopts: name must not be empty\n");
        return 2;
    }
    
    char name[64];
    if (name_len >= sizeof(name)) name_len = sizeof(name) - 1;
    for (size_t i = 0; i < name_len; i++) name[i] = name_start[i];
    name[name_len] = '\0';
    
    // Get OPTIND from environment, default to 1
    const char* optind_str = s ? jash_env_get(s, "OPTIND") : NULL;
    int argument_index = optind_str ? atoi(optind_str) : 1;
    /* SECURITY (audit rank 3, heap OOB read + wild deref): OPTIND is
     * user-controlled (`OPTIND=0 getopts ...` / `OPTIND=-9`). The upper bound
     * is checked below (argument_index > argc) but the lower bound was not, so
     * argv[argument_index - 1] indexed argv[-1], argv[-10], ... → out-of-bounds read
     * and a wild-pointer deref at arg[0]. POSIX OPTIND is >= 1; clamp it. */
    if (argument_index < 1) argument_index = 1;
    
    // Get the arguments to parse
    p = skip_ws(p);
    const char* args_to_parse = p ? p : "";
    
    // If explicit arguments provided, use them; otherwise use positional parameters
    const char* parse_args = args_to_parse;
    int explicit_args = (args_to_parse && *args_to_parse);
    
    // Get the argument array (positional parameters or explicit args)
    char** argv = NULL;
    int argc = 0;
    
    if (explicit_args) {
        // Parse explicit arguments string into argv
        // Simple implementation: split by spaces (not handling quotes)
        int capacity = 16;
        argv = malloc(capacity * sizeof(char*));
        if (!argv) return 2;
        
        const char* arg_start = parse_args;
        while (*parse_args) {
            // Skip spaces
            while (*parse_args == ' ' || *parse_args == '\t') parse_args++;
            if (!*parse_args) break;
            
            arg_start = parse_args;
            while (*parse_args && *parse_args != ' ' && *parse_args != '\t') parse_args++;
            
            size_t arg_len = parse_args - arg_start;
            if (arg_len > 0) {
                if (argc >= capacity) {
                    capacity *= 2;
                    argv = realloc(argv, capacity * sizeof(char*));
                    if (!argv) return 2;
                }
                argv[argc] = malloc(arg_len + 1);
                if (!argv[argc]) return 2;
                memcpy(argv[argc], arg_start, arg_len);
                argv[argc][arg_len] = '\0';
                argc++;
            }
        }
        
        if (argc == 0) {
            // No arguments provided
            free(argv);
            argv = NULL;
        }
    } else {
        // Use positional parameters ($1, $2, ...)
        // Count how many we have
        argc = 0;
        while (1) {
            char key[32];
            snprintf(key, sizeof(key), "%d", argc + 1);
            const char* val = s ? jash_env_get(s, key) : NULL;
            if (!val) break;
            argc++;
        }
        
        if (argc > 0) {
            argv = malloc(argc * sizeof(char*));
            if (!argv) return 2;
            
            for (int i = 0; i < argc; i++) {
                char key[32];
                snprintf(key, sizeof(key), "%d", i + 1);
                const char* val = s ? jash_env_get(s, key) : NULL;
                if (val) {
                    size_t len = strlen(val);
                    argv[i] = malloc(len + 1);
                    if (!argv[i]) {
                        // Cleanup
                        for (int j = 0; j < i; j++) free(argv[j]);
                        free(argv);
                        return 2;
                    }
                    memcpy(argv[i], val, len + 1);
                } else {
                    argv[i] = strdup("");
                    if (!argv[i]) {
                        // Cleanup
                        for (int j = 0; j < i; j++) free(argv[j]);
                        free(argv);
                        return 2;
                    }
                }
            }
        }
    }
    
    // Check if we have enough arguments
    if (argument_index > argc) {
        // No more arguments to process
        if (explicit_args && argv) {
            for (int i = 0; i < argc; i++) free(argv[i]);
            free(argv);
        }
        jash_env_set(s, name, "?");
        jash_env_set(s, "OPTARG", "");
        // OPTIND remains at its current value (which is > argc)
        return 1;
    }
    
    // Get current argument
    const char* arg = argv[argument_index - 1];
    
    // Check if argument starts with '-'
    if (!arg || arg[0] != '-' || !arg[1]) {
        // No more options
        if (explicit_args && argv) {
            for (int i = 0; i < argc; i++) free(argv[i]);
            free(argv);
        }
        jash_env_set(s, name, "?");
        jash_env_set(s, "OPTARG", "");
        // OPTIND remains at its current value
        return 1;
    }
    
    // Move past the '-'
    const char* opt = arg + 1;
    
    // Check for '--' (end of options)
    if (opt[0] == '-' && !opt[1]) {
        // End of options
        argument_index++;
        if (explicit_args && argv) {
            for (int i = 0; i < argc; i++) free(argv[i]);
            free(argv);
        }
        jash_env_set(s, name, "?");
        jash_env_set(s, "OPTARG", "");
        return 1;
    }
    
    // Find the option character in optstring
    const char* opt_pos = strchr(optstring, opt[0]);
    if (!opt_pos) {
        // Unknown option
        jash_env_set(s, name, "?");
        jash_env_set(s, "OPTARG", &opt[0]);
        // Print error message if optstring doesn't start with ':'
        if (optstring[0] != ':') {
            jash_writef(w, ctx, "getopts: illegal option -- %c\n", opt[0]);
        }
        argument_index++;
        if (explicit_args && argv) {
            for (int i = 0; i < argc; i++) free(argv[i]);
            free(argv);
        }
        return 1;
    }
    
    // Check if option requires an argument
    if (opt_pos[1] == ':') {
        // Option requires an argument
        if (opt[1]) {
            // Argument is in the same word (e.g., -oarg)
            jash_env_set(s, name, &opt[0]);
            char* option_argument = strdup(&opt[1]);
            jash_env_set(s, "OPTARG", option_argument);
            free(option_argument);
            argument_index++;
        } else {
            // Need next argument
            argument_index++;
            if (argument_index > argc) {
                // Missing argument
                jash_env_set(s, name, "?");
                jash_env_set(s, "OPTARG", &opt[0]);
                if (optstring[0] == ':') {
                    // Silent mode
                } else {
                jash_writef(w, ctx, "getopts: option requires an argument -- %c\n", opt[0]);
                }
                if (explicit_args && argv) {
                    for (int i = 0; i < argc; i++) free(argv[i]);
                    free(argv);
                }
                return 1;
            }
            // Get the argument
            const char* option_argument = argv[argument_index - 1];
            jash_env_set(s, name, &opt[0]);
            jash_env_set(s, "OPTARG", option_argument);
            argument_index++;
        }
    } else {
        // Option does not require an argument
        char opt_char[2] = {opt[0], '\0'};
        jash_env_set(s, name, opt_char);
        jash_env_set(s, "OPTARG", "");
        if (opt[1]) {
            // More characters in this argument
            // For now, we'll treat the rest as a new argument (simplified)
            // In a full implementation, we'd need to handle this properly
            argument_index++;
        } else {
            argument_index++;
        }
    }
    
    // Update OPTIND in environment
    char optind_buf[32];
    snprintf(optind_buf, sizeof(optind_buf), "%d", argument_index);
    jash_env_set(s, "OPTIND", optind_buf);
    
    // Cleanup
    if (explicit_args && argv) {
        for (int i = 0; i < argc; i++) free(argv[i]);
        free(argv);
    }
    
    return 0;
}
