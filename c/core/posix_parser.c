// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// POSIX-compliant parser

#include "posix_parser.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Token types
typedef enum {
    TOK_WORD,
    TOK_PIPE,        // |
    TOK_SEMICOLON,   // ;
    TOK_AMPERSAND,   // &
    TOK_AND,         // &&
    TOK_OR,          // ||
    TOK_LPAREN,      // (
    TOK_RPAREN,      // )
    TOK_REDIR_IN,    // <
    TOK_REDIR_OUT,   // >
    TOK_REDIR_APPEND,// >>
    TOK_REDIR_ERR,   // 2>
    TOK_REDIR_ERR_APPEND, // 2>>
    TOK_REDIR_BOTH,  // &>
    TOK_EOF
} token_type_t;

typedef struct {
    token_type_t type;
    char* value;
    int pos;
} token_t;

typedef struct {
    const char* input;
    int pos;
    int len;
} lexer_t;

static void lexer_init(lexer_t* lex, const char* input) {
    lex->input = input;
    lex->pos = 0;
    lex->len = strlen(input);
}

static char lexer_peek(lexer_t* lex) {
    if (lex->pos >= lex->len) return '\0';
    return lex->input[lex->pos];
}

static char lexer_next(lexer_t* lex) {
    if (lex->pos >= lex->len) return '\0';
    return lex->input[lex->pos++];
}

static void skip_whitespace(lexer_t* lex) {
    while (lex->pos < lex->len && isspace(lex->input[lex->pos])) {
        lex->pos++;
    }
}

// Skip comments
static void skip_comment(lexer_t* lex) {
    if (lexer_peek(lex) == '#') {
        while (lex->pos < lex->len && lex->input[lex->pos] != '\n') {
            lex->pos++;
        }
    }
}

// Read a word (handles quoting)
static char* read_word(lexer_t* lex) {
    char buf[4096];
    int len = 0;
    
    while (lex->pos < lex->len) {
        char c = lexer_peek(lex);
        
        if (isspace(c) || c == '|' || c == ';' || c == '&' || 
            c == '<' || c == '>' || c == '(' || c == ')') {
            break;
        }
        
        // Handle quotes
        if (c == '\'' || c == '"') {
            char quote = lexer_next(lex);
            while (lex->pos < lex->len && lexer_peek(lex) != quote) {
                if (len < 4095) {
                    buf[len++] = lexer_next(lex);
                }
            }
            if (lex->pos < lex->len) {
                lexer_next(lex);  // Skip closing quote
            }
        } else if (c == '\\') {
            lexer_next(lex);  // Skip backslash
            if (lex->pos < lex->len) {
                if (len < 4095) {
                    buf[len++] = lexer_next(lex);
                }
            }
        } else {
            if (len < 4095) {
                buf[len++] = lexer_next(lex);
            }
        }
    }
    
    buf[len] = '\0';
    return strdup(buf);
}

// Get next token
static token_t next_token(lexer_t* lex) {
    skip_whitespace(lex);
    skip_comment(lex);
    skip_whitespace(lex);
    
    token_t tok = {0};
    tok.pos = lex->pos;
    
    if (lex->pos >= lex->len) {
        tok.type = TOK_EOF;
        return tok;
    }
    
    char c = lexer_peek(lex);
    
    // Check for operators
    if (c == '|') {
        lexer_next(lex);
        if (lexer_peek(lex) == '|') {
            lexer_next(lex);
            tok.type = TOK_OR;
            tok.value = strdup("||");
        } else {
            tok.type = TOK_PIPE;
            tok.value = strdup("|");
        }
        return tok;
    }
    
    if (c == '&') {
        lexer_next(lex);
        if (lexer_peek(lex) == '&') {
            lexer_next(lex);
            tok.type = TOK_AND;
            tok.value = strdup("&&");
        } else if (lexer_peek(lex) == '>') {
            lexer_next(lex);
            tok.type = TOK_REDIR_BOTH;
            tok.value = strdup("&>");
        } else {
            tok.type = TOK_AMPERSAND;
            tok.value = strdup("&");
        }
        return tok;
    }
    
    if (c == ';') {
        lexer_next(lex);
        tok.type = TOK_SEMICOLON;
        tok.value = strdup(";");
        return tok;
    }
    
    if (c == '(') {
        lexer_next(lex);
        tok.type = TOK_LPAREN;
        tok.value = strdup("(");
        return tok;
    }
    
    if (c == ')') {
        lexer_next(lex);
        tok.type = TOK_RPAREN;
        tok.value = strdup(")");
        return tok;
    }
    
    if (c == '<') {
        lexer_next(lex);
        tok.type = TOK_REDIR_IN;
        tok.value = strdup("<");
        return tok;
    }
    
    if (c == '>') {
        lexer_next(lex);
        if (lexer_peek(lex) == '>') {
            lexer_next(lex);
            tok.type = TOK_REDIR_APPEND;
            tok.value = strdup(">>");
        } else {
            tok.type = TOK_REDIR_OUT;
            tok.value = strdup(">");
        }
        return tok;
    }
    
    // Check for 2> or 2>>
    if (c == '2' && lex->pos + 1 < lex->len && lex->input[lex->pos + 1] == '>') {
        lexer_next(lex);  // Skip '2'
        lexer_next(lex);  // Skip '>'
        if (lexer_peek(lex) == '>') {
            lexer_next(lex);
            tok.type = TOK_REDIR_ERR_APPEND;
            tok.value = strdup("2>>");
        } else {
            tok.type = TOK_REDIR_ERR;
            tok.value = strdup("2>");
        }
        return tok;
    }
    
    // Otherwise, it's a word
    tok.type = TOK_WORD;
    tok.value = read_word(lex);
    return tok;
}

// Parse a simple command
static posix_command_t* parse_simple_command(lexer_t* lex) {
    posix_command_t* cmd = calloc(1, sizeof(posix_command_t));
    cmd->argc = 0;
    cmd->argv = calloc(256, sizeof(char*));
    cmd->nredirs = 0;
    cmd->redirs = calloc(32, sizeof(posix_redirect_t));
    cmd->background = false;
    
    token_t tok = next_token(lex);
    
    while (tok.type == TOK_WORD) {
        if (cmd->argc < 255) {
            cmd->argv[cmd->argc++] = tok.value;
        } else {
            free(tok.value);
        }
        
        tok = next_token(lex);
        
        // Check for redirections
        if (tok.type >= TOK_REDIR_IN && tok.type <= TOK_REDIR_BOTH) {
            if (cmd->nredirs < 31) {
                posix_redirect_t* redir = &cmd->redirs[cmd->nredirs];
                redir->type = (posix_redir_type_t)(tok.type - TOK_REDIR_IN + REDIR_IN);
                free(tok.value);
                
                tok = next_token(lex);
                if (tok.type == TOK_WORD) {
                    redir->filename = tok.value;
                    cmd->nredirs++;
                } else {
                    fprintf(stderr, "jash: syntax error near unexpected token\n");
                    if (tok.value) free(tok.value);
                }
            }
            tok = next_token(lex);
        }
    }
    
    cmd->argv[cmd->argc] = NULL;
    
    // Check for background
    if (tok.type == TOK_AMPERSAND) {
        cmd->background = true;
        free(tok.value);
    } else {
        // Put token back (simplified - in real impl would use lookahead)
        if (tok.value) free(tok.value);
    }
    
    return cmd;
}

// Parse a pipeline
static posix_pipeline_t* parse_pipeline(lexer_t* lex) {
    posix_pipeline_t* pipeline = calloc(1, sizeof(posix_pipeline_t));
    pipeline->ncmds = 0;
    pipeline->cmds = calloc(64, sizeof(posix_command_t*));
    
    pipeline->cmds[pipeline->ncmds++] = parse_simple_command(lex);
    
    token_t tok = next_token(lex);
    while (tok.type == TOK_PIPE) {
        free(tok.value);
        if (pipeline->ncmds < 63) {
            pipeline->cmds[pipeline->ncmds++] = parse_simple_command(lex);
        }
        tok = next_token(lex);
    }
    
    // Put token back
    if (tok.value) free(tok.value);
    
    return pipeline;
}

// Parse a command list
posix_command_list_t* posix_parse(const char* input) {
    lexer_t lex;
    lexer_init(&lex, input);
    
    posix_command_list_t* list = calloc(1, sizeof(posix_command_list_t));
    list->npipelines = 0;
    list->pipelines = calloc(256, sizeof(posix_pipeline_t*));
    list->operators = calloc(256, sizeof(int));
    
    list->pipelines[list->npipelines++] = parse_pipeline(&lex);
    
    token_t tok = next_token(&lex);
    while (tok.type == TOK_AND || tok.type == TOK_OR || tok.type == TOK_SEMICOLON) {
        list->operators[list->npipelines - 1] = tok.type;
        free(tok.value);
        
        if (list->npipelines < 255) {
            list->pipelines[list->npipelines++] = parse_pipeline(&lex);
        }
        
        tok = next_token(&lex);
    }
    
    if (tok.value) free(tok.value);
    
    return list;
}

// Free command list
void posix_free(posix_command_list_t* list) {
    if (!list) return;
    
    for (int i = 0; i < list->npipelines; i++) {
        posix_pipeline_t* pipeline = list->pipelines[i];
        if (!pipeline) continue;
        
        for (int j = 0; j < pipeline->ncmds; j++) {
            posix_command_t* cmd = pipeline->cmds[j];
            if (!cmd) continue;
            
            for (int k = 0; k < cmd->argc; k++) {
                free(cmd->argv[k]);
            }
            free(cmd->argv);
            
            for (int k = 0; k < cmd->nredirs; k++) {
                free(cmd->redirs[k].filename);
            }
            free(cmd->redirs);
            
            free(cmd);
        }
        free(pipeline->cmds);
        free(pipeline);
    }
    
    free(list->pipelines);
    free(list->operators);
    free(list);
}
