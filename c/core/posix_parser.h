// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// POSIX-compliant parser header

#ifndef JASH_POSIX_PARSER_H
#define JASH_POSIX_PARSER_H

#include <stdbool.h>

// Redirection types
typedef enum {
    REDIR_IN = 1,      // <
    REDIR_OUT,         // >
    REDIR_APPEND,      // >>
    REDIR_ERR,         // 2>
    REDIR_ERR_APPEND,  // 2>>
    REDIR_BOTH         // &>
} posix_redir_type_t;

// Redirection
typedef struct {
    posix_redir_type_t type;
    char* filename;
} posix_redirect_t;

// Simple command
typedef struct {
    int argc;
    char** argv;
    int nredirs;
    posix_redirect_t* redirs;
    bool background;
} posix_command_t;

// Pipeline (commands connected by |)
typedef struct {
    int ncmds;
    posix_command_t** cmds;
} posix_pipeline_t;

// Command list (pipelines connected by &&, ||, ;)
typedef struct {
    int npipelines;
    posix_pipeline_t** pipelines;
    int* operators;  // TOK_AND, TOK_OR, TOK_SEMICOLON
} posix_command_list_t;

// Parse input string into command list
posix_command_list_t* posix_parse(const char* input);

// Free command list
void posix_free(posix_command_list_t* list);

#endif // JASH_POSIX_PARSER_H
