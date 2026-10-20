/* SPDX-License-Identifier: LicenseRef-SPL-1.0 */
// Native C/C++ subset lexer and AST extractor used by the living graph.
// Directives are skipped, not expanded; this is not a validating C compiler.
#include "jash_graph.h"
#include "jash_parse.h"
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static uint32_t tok_idx = 0;

static JSH_Token *current(JSH_ParseContext *ctx) {
    return &ctx->tokens[tok_idx];
}

static JSH_Token *advance(JSH_ParseContext *ctx) {
    return &ctx->tokens[tok_idx++];
}

static int is_token(JSH_ParseContext *ctx, JSH_TokenType t) {
    JSH_Token *cur = current(ctx);
    return cur->type == t;
}

static int check_name(JSH_ParseContext *ctx, const char *name) {
    JSH_Token *t = current(ctx);
    return t->length > 0 && t->type != JSH_TOKEN_EOF &&
           strncmp(&ctx->source_buffer[t->start], name, t->length) == 0;
}

static int consume_punct(JSH_ParseContext *ctx, char expected) {
    JSH_Token *t = current(ctx);
    if (t->type == JSH_TOKEN_DELIMITER_PUNCT &&
        ctx->source_buffer[t->start] == expected) {
        advance(ctx);
        return 1;
    }
    return 0;
}

JSH_Node *jsh_node_create(JSH_ParseContext *ctx, JSH_NodeType type, const char *filename) {
    if (!ctx || ctx->node_count >= ctx->node_capacity) return NULL;
    
    JSH_Node *n = &ctx->nodes[ctx->node_count++];
    memset(n, 0, sizeof(JSH_Node));
    n->type = type;
    if (filename) {
        n->filename = filename;
    }
    return n;
}

JSH_Node *jsh_node_add_child(JSH_ParseContext *ctx, JSH_Node *parent, JSH_Node *child) {
    if (!parent || !child) return NULL;
    
    if (parent->child_count >= parent->child_capacity) {
        uint32_t new_cap = parent->child_capacity ? parent->child_capacity * 2 : 4;
        JSH_Node **new_children = (JSH_Node **)realloc(parent->children, 
                                                        new_cap * sizeof(JSH_Node *));
        if (!new_children) return NULL;
        parent->children = new_children;
        parent->child_capacity = new_cap;
    }
    
    parent->children[parent->child_count++] = child;
    return child;
}

void jsh_node_set_extent(JSH_ParseContext *ctx, JSH_Node *n, uint32_t start, uint32_t end) {
    if (!n || !ctx) return;
    n->start_byte = start;
    n->end_byte = end;
    (void)ctx;
}

int jsh_tokenize_all(JSH_ParseContext *ctx) {
    tok_idx = 0;
    ctx->token_count = 0;
    ctx->current_token = 0;
    if (!ctx->tokens || !ctx->token_capacity) return -1;
    uint32_t line_no = 1;
    
    while (ctx->source_buffer && ctx->source_buffer[ctx->current_token]) {
        unsigned char c = (unsigned char)ctx->source_buffer[ctx->current_token];
        if (isspace(c)) {
            if (c == '\n') line_no++;
            ctx->current_token++;
            continue;
        }
        
        JSH_Token tk = {0};
        tk.start = ctx->current_token;
        tk.line = line_no;
        
        if (c == '/' && ctx->source_buffer[ctx->current_token + 1] == '/') {
            ctx->current_token += 2;
            for (;;) {
                while (ctx->source_buffer[ctx->current_token] &&
                       ctx->source_buffer[ctx->current_token] != '\n') {
                    ctx->current_token++;
                }
                /* ADR-0053: backslash-newline splices the next physical line
                 * into this comment (C11 5.1.1.2 phase 2) — keep skipping */
                if (ctx->current_token > 0 &&
                    ctx->source_buffer[ctx->current_token - 1] == '\\' &&
                    ctx->source_buffer[ctx->current_token] == '\n') {
                    ctx->current_token++;
                    line_no++;
                    continue;
                }
                break;
            }
            continue;
        }
        
        if (c == '/' && ctx->source_buffer[ctx->current_token + 1] == '*') {
            ctx->current_token += 2;
            while (ctx->source_buffer[ctx->current_token] &&
                   !(ctx->source_buffer[ctx->current_token] == '*' &&
                     ctx->source_buffer[ctx->current_token + 1] == '/')) {
                if (ctx->source_buffer[ctx->current_token] == '\n') line_no++;
                ctx->current_token++;
            }
            if (ctx->source_buffer[ctx->current_token]) {
                ctx->current_token += 2;
            } else {
                snprintf(ctx->error_msg, sizeof(ctx->error_msg), "unterminated block comment");
                ctx->had_error = 1;
                break;
            }
            continue;
        }
        
        if (c == '#') {
            ctx->current_token++;
            /* ADR-0053: a directive with backslash-newline continuations is
             * ONE logical line — skipping only to the first '\n' left the
             * continuation body to be tokenized as code, which desynced the
             * parser through the end of the file (measured: c/main.c lost
             * every function past a multi-line #define, main() included). */
            for (;;) {
                while (ctx->source_buffer[ctx->current_token] &&
                       ctx->source_buffer[ctx->current_token] != '\n') {
                    ctx->current_token++;
                }
                if (ctx->current_token > 0 &&
                    ctx->source_buffer[ctx->current_token - 1] == '\\' &&
                    ctx->source_buffer[ctx->current_token] == '\n') {
                    ctx->current_token++;
                    line_no++;
                    continue;
                }
                break;
            }
            continue;
        }
        
        if (isalpha(c) || c == '_') {
            while (isalnum((unsigned char)ctx->source_buffer[ctx->current_token]) ||
                   ctx->source_buffer[ctx->current_token] == '_' ||
                   (unsigned char)ctx->source_buffer[ctx->current_token] > 127) {
                ctx->current_token++;
            }
            tk.length = ctx->current_token - tk.start;
            
            if (tk.length == 6 && strncmp(&ctx->source_buffer[tk.start], "return", 6) == 0) {
                tk.type = JSH_TOKEN_KEYWORD_VALUE;
            } else if (tk.length == 4 && strncmp(&ctx->source_buffer[tk.start], "else", 4) == 0) {
                tk.type = JSH_TOKEN_KEYWORD_CONTROL;
            } else if (tk.length == 2 && strncmp(&ctx->source_buffer[tk.start], "if", 2) == 0) {
                tk.type = JSH_TOKEN_KEYWORD_CONTROL;
            } else if (tk.length == 5 && strncmp(&ctx->source_buffer[tk.start], "while", 5) == 0) {
                tk.type = JSH_TOKEN_KEYWORD_CONTROL;
            } else if (tk.length == 3 && strncmp(&ctx->source_buffer[tk.start], "for", 3) == 0) {
                tk.type = JSH_TOKEN_KEYWORD_CONTROL;
            } else if (tk.length == 4 && strncmp(&ctx->source_buffer[tk.start], "void", 4) == 0) {
                tk.type = JSH_TOKEN_KEYWORD_TYPE;
            } else if (tk.length == 3 && strncmp(&ctx->source_buffer[tk.start], "int", 3) == 0) {
                tk.type = JSH_TOKEN_KEYWORD_TYPE;
            } else if (tk.length == 4 && strncmp(&ctx->source_buffer[tk.start], "char", 4) == 0) {
                tk.type = JSH_TOKEN_KEYWORD_TYPE;
            } else if (tk.length == 5 && strncmp(&ctx->source_buffer[tk.start], "float", 5) == 0) {
                tk.type = JSH_TOKEN_KEYWORD_TYPE;
            } else if (tk.length == 6 && strncmp(&ctx->source_buffer[tk.start], "double", 6) == 0) {
                tk.type = JSH_TOKEN_KEYWORD_TYPE;
            } else if (tk.length == 6 && strncmp(&ctx->source_buffer[tk.start], "struct", 6) == 0) {
                tk.type = JSH_TOKEN_KEYWORD_TYPE;
            } else if (tk.length == 5 && strncmp(&ctx->source_buffer[tk.start], "union", 5) == 0) {
                tk.type = JSH_TOKEN_KEYWORD_TYPE;
            } else if (tk.length == 4 && strncmp(&ctx->source_buffer[tk.start], "enum", 4) == 0) {
                tk.type = JSH_TOKEN_KEYWORD_TYPE;
            } else if (tk.length == 6 && strncmp(&ctx->source_buffer[tk.start], "static", 6) == 0) {
                tk.type = JSH_TOKEN_KEYWORD_STORAGE;
            } else if (tk.length == 6 && strncmp(&ctx->source_buffer[tk.start], "extern", 6) == 0) {
                tk.type = JSH_TOKEN_KEYWORD_STORAGE;
            } else if (tk.length == 7 && strncmp(&ctx->source_buffer[tk.start], "typedef", 7) == 0) {
                tk.type = JSH_TOKEN_KEYWORD_STORAGE;
            } else if (tk.length == 5 && strncmp(&ctx->source_buffer[tk.start], "const", 5) == 0) {
                tk.type = JSH_TOKEN_KEYWORD_STORAGE;
            } else if (tk.length == 7 && strncmp(&ctx->source_buffer[tk.start], "signed", 7) == 0) {
                tk.type = JSH_TOKEN_KEYWORD_TYPE;
            } else if (tk.length == 8 && strncmp(&ctx->source_buffer[tk.start], "unsigned", 8) == 0) {
                tk.type = JSH_TOKEN_KEYWORD_TYPE;
            } else if (tk.length == 4 && strncmp(&ctx->source_buffer[tk.start], "long", 4) == 0) {
                tk.type = JSH_TOKEN_KEYWORD_TYPE;
            } else if (tk.length == 5 && strncmp(&ctx->source_buffer[tk.start], "short", 5) == 0) {
                tk.type = JSH_TOKEN_KEYWORD_TYPE;
            } else {
                tk.type = JSH_TOKEN_IDENTIFIER;
            }
        } else if (isdigit(c)) {
            tk.type = JSH_TOKEN_NUMBER_INT;
            while (isdigit(ctx->source_buffer[ctx->current_token])) {
                ctx->current_token++;
            }
            tk.length = ctx->current_token - tk.start;
        } else if (c == '"' || c == '\'') {
            tk.type = c == '"' ? JSH_TOKEN_STRING_LITERAL : JSH_TOKEN_CHAR_LITERAL;
            ctx->current_token++;
            tk.start = ctx->current_token;
            while (ctx->source_buffer[ctx->current_token] != c &&
                    ctx->source_buffer[ctx->current_token] != '\0') {
                if (ctx->source_buffer[ctx->current_token] == '\\') {
                    ctx->current_token++;
                    if (!ctx->source_buffer[ctx->current_token]) break;
                }
                if (ctx->source_buffer[ctx->current_token] == '\n') line_no++;
                ctx->current_token++;
            }
            if (!ctx->source_buffer[ctx->current_token]) {
                snprintf(ctx->error_msg, sizeof(ctx->error_msg), "unterminated quoted literal");
                ctx->had_error = 1;
                break;
            }
            tk.length = ctx->current_token - tk.start;
            ctx->current_token++;
        } else if (c == '&' && ctx->source_buffer[ctx->current_token + 1] == '&') {
            tk.type = JSH_TOKEN_OPERATOR_LOGIC;
            tk.length = 2;
            ctx->current_token += 2;
        } else if (c == '|' && ctx->source_buffer[ctx->current_token + 1] == '|') {
            tk.type = JSH_TOKEN_OPERATOR_LOGIC;
            tk.length = 2;
            ctx->current_token += 2;
        } else if (strchr("+-*/%~", c)) {
            tk.type = JSH_TOKEN_OPERATOR_ARITH;
            tk.length = 1;
            ctx->current_token++;
        } else if (strchr("&|^", c)) {
            if ((c == '<' || c == '>') && ctx->source_buffer[ctx->current_token + 1] == c) {
                tk.type = JSH_TOKEN_OPERATOR_BITWISE;
                tk.length = 2;
                ctx->current_token += 2;
            } else {
                tk.type = JSH_TOKEN_OPERATOR_BITWISE;
                tk.length = 1;
                ctx->current_token++;
            }
        } else if (strchr("=<>!:", c)) {
            if (c == ':' ) {
                tk.type = JSH_TOKEN_OPERATOR_ASSIGN;
                tk.length = 1;
                ctx->current_token++;
            } else {
                tk.type = JSH_TOKEN_OPERATOR_ASSIGN;
                if (ctx->source_buffer[ctx->current_token + 1] == '=') {
                    tk.length = 2;
                    ctx->current_token += 2;
                } else {
                    tk.length = 1;
                    ctx->current_token++;
                }
            }
        } else if (c == '?') {
            tk.type = JSH_TOKEN_OPERATOR_LOGIC;
            tk.length = 1;
            ctx->current_token++;
        } else if (c == '(' || c == ')') {
            tk.type = JSH_TOKEN_DELIMITER_BRACKET;
            tk.length = 1;
            ctx->current_token++;
        } else if (c == ')') {
            tk.type = JSH_TOKEN_DELIMITER_BRACKET;
            tk.length = 1;
            ctx->current_token++;
        } else if (c == '[') {
            tk.type = JSH_TOKEN_DELIMITER_BRACKET;
            tk.length = 1;
            ctx->current_token++;
        } else if (c == ']') {
            tk.type = JSH_TOKEN_DELIMITER_BRACKET;
            tk.length = 1;
            ctx->current_token++;
        } else if (strchr(";,.:?{}", c)) {
            tk.type = JSH_TOKEN_DELIMITER_PUNCT;
            tk.length = 1;
            ctx->current_token++;
        } else {
            snprintf(ctx->error_msg, sizeof(ctx->error_msg), "unsupported byte 0x%02x", c);
            ctx->had_error = 1;
            break;
        }
        
        /* ADR-0053: respect token_capacity — a file past the cap stops
         * tokenizing here instead of corrupting the heap (the trailing EOF
         * token below still lands in-bounds). */
        if (ctx->token_count + 1 >= ctx->token_capacity) {
            snprintf(ctx->error_msg, sizeof(ctx->error_msg), "token capacity exceeded");
            ctx->had_error = 1;
            break;
        }
        ctx->tokens[ctx->token_count++] = tk;
    }
    
    ctx->tokens[ctx->token_count] = (JSH_Token){0};
    ctx->tokens[ctx->token_count].type = JSH_TOKEN_EOF;
    ctx->token_count++;
    return ctx->had_error ? -1 : 0;
}

static JSH_Node *parse_primary(JSH_ParseContext *ctx) {
    JSH_Token *t = current(ctx);
    if (t->type == JSH_TOKEN_NUMBER_INT ||
        t->type == JSH_TOKEN_STRING_LITERAL ||
        t->type == JSH_TOKEN_IDENTIFIER) {
        JSH_Node *n = jsh_node_create(ctx, JSH_NODE_UNARY_EXPR, ctx->file_path);
        n->name = *advance(ctx);
        if (n->name.type == JSH_TOKEN_IDENTIFIER) {
            strncpy(n->name_buffer, &ctx->source_buffer[n->name.start], n->name.length);
            n->name_buffer[n->name.length] = '\0';
        }
        return n;
    }
    
    if (is_token(ctx, JSH_TOKEN_DELIMITER_BRACKET) &&
        ctx->source_buffer[current(ctx)->start] == '(') {
        advance(ctx);
        JSH_Node *e = jsh_parse_expression(ctx, 0);
        if (!(is_token(ctx, JSH_TOKEN_DELIMITER_BRACKET) &&
              ctx->source_buffer[current(ctx)->start] == ')')) {
            return e;
        }
        advance(ctx);
        return e;
    }
    
    return NULL;
}

static JSH_Node *parse_binary(JSH_ParseContext *ctx, int min_bp) {
    JSH_Node *lhs = parse_primary(ctx);
    if (!lhs) return NULL;
    
    while (current(ctx)->type != JSH_TOKEN_EOF &&
           current(ctx)->type != JSH_TOKEN_DELIMITER_PUNCT &&
           current(ctx)->type != JSH_TOKEN_DELIMITER_BRACKET) {
        int bp = 0;
        switch (current(ctx)->type) {
            case JSH_TOKEN_OPERATOR_ASSIGN:
            case JSH_TOKEN_OPERATOR_COMPARE:
                bp = 10;
                break;
            case JSH_TOKEN_OPERATOR_BITWISE:
                bp = 15;
                break;
            case JSH_TOKEN_OPERATOR_ARITH:
                bp = 20;
                break;
            case JSH_TOKEN_OPERATOR_LOGIC:
                bp = 25;
                break;
            default:
                return lhs;
        }
        
        if (bp < min_bp) break;
        
        advance(ctx);
        
        JSH_Node *rhs = parse_binary(ctx, bp + 1);
        if (!rhs) {
            rhs = jsh_node_create(ctx, JSH_NODE_UNARY_EXPR, ctx->file_path);
        }
        
        JSH_Node *bin = jsh_node_create(ctx, JSH_NODE_BINARY_EXPR, ctx->file_path);
        bin->child_capacity = 2;
        bin->children = (JSH_Node **)malloc(2 * sizeof(JSH_Node *));
        bin->children[0] = lhs;
        bin->children[1] = rhs;
        bin->child_count = 2;
        lhs = bin;
    }
    
    return lhs;
}

JSH_Node *jsh_parse_expression(JSH_ParseContext *ctx, int prec) {
    return parse_binary(ctx, prec);
}

/* ADR-0053: skip a balanced { … } body at token level ('{'/'}' are
 * DELIMITER_PUNCT in this tokenizer). Current token must be the '{'. */
static void skip_brace_body(JSH_ParseContext *ctx) {
    int depth = 0;
    while (!is_token(ctx, JSH_TOKEN_EOF)) {
        JSH_Token *t = current(ctx);
        if (t->type == JSH_TOKEN_DELIMITER_PUNCT) {
            char c = ctx->source_buffer[t->start];
            if (c == '{') depth++;
            else if (c == '}') {
                depth--;
                advance(ctx);
                if (depth <= 0) return;
                continue;
            }
        }
        advance(ctx);
    }
}

JSH_Node *jsh_parse_statement(JSH_ParseContext *ctx) {
    if (is_token(ctx, JSH_TOKEN_KEYWORD_STORAGE) || is_token(ctx, JSH_TOKEN_KEYWORD_TYPE)) {
        /* ADR-0053: remember WHY we entered — typedef/struct/union/enum
         * declarations become named graph nodes, not just VAR_DECLARATIONs. */
        int saw_typedef = 0;
        JSH_NodeType sue_type = JSH_NODE_PROGRAM;
        while (is_token(ctx, JSH_TOKEN_KEYWORD_STORAGE) || is_token(ctx, JSH_TOKEN_KEYWORD_TYPE)) {
            JSH_Token *kw = current(ctx);
            const char *ks = &ctx->source_buffer[kw->start];
            if (kw->length == 7 && strncmp(ks, "typedef", 7) == 0) saw_typedef = 1;
            else if (kw->length == 6 && strncmp(ks, "struct", 6) == 0) sue_type = JSH_NODE_STRUCT_DECLARATION;
            else if (kw->length == 5 && strncmp(ks, "union", 5) == 0) sue_type = JSH_NODE_UNION_DECLARATION;
            else if (kw->length == 4 && strncmp(ks, "enum", 4) == 0) sue_type = JSH_NODE_ENUM_DECLARATION;
            advance(ctx);
        }
        
        while (is_token(ctx, JSH_TOKEN_OPERATOR_ARITH) &&
               (ctx->source_buffer[current(ctx)->start] == '*' ||
                ctx->source_buffer[current(ctx)->start] == '&' ||
                ctx->source_buffer[current(ctx)->start] == '~')) {
            advance(ctx);
        }
        
        /* anonymous body: typedef struct { … } Name;  (only a typedef names it) */
        if (sue_type != JSH_NODE_PROGRAM &&
            is_token(ctx, JSH_TOKEN_DELIMITER_PUNCT) &&
            ctx->source_buffer[current(ctx)->start] == '{') {
            skip_brace_body(ctx);
            JSH_Node *td = NULL;
            if (saw_typedef && is_token(ctx, JSH_TOKEN_IDENTIFIER)) {
                td = jsh_node_create(ctx, JSH_NODE_TYPEDEF_DECLARATION, ctx->file_path);
                td->name = *current(ctx);
                td->start_line = current(ctx)->line;
                advance(ctx);
            }
            while (!is_token(ctx, JSH_TOKEN_EOF) &&
                   !(is_token(ctx, JSH_TOKEN_DELIMITER_PUNCT) &&
                     ctx->source_buffer[current(ctx)->start] == ';')) {
                advance(ctx);
            }
            if (is_token(ctx, JSH_TOKEN_DELIMITER_PUNCT) &&
                ctx->source_buffer[current(ctx)->start] == ';') {
                advance(ctx);
            }
            return td;
        }
        
        if (is_token(ctx, JSH_TOKEN_IDENTIFIER)) {
                size_t name_start = current(ctx)->start;
            size_t name_len = current(ctx)->length;
            uint32_t name_line = current(ctx)->line;
            advance(ctx);
            
            /* named struct/union/enum body: struct point { … }; */
            if (sue_type != JSH_NODE_PROGRAM &&
                is_token(ctx, JSH_TOKEN_DELIMITER_PUNCT) &&
                ctx->source_buffer[current(ctx)->start] == '{') {
                JSH_Node *sue = jsh_node_create(ctx, sue_type, ctx->file_path);
                sue->name.start = name_start;
                sue->name.length = name_len;
                sue->start_line = name_line;
                skip_brace_body(ctx);
                /* optional declarator list after the body: struct p {…} v; */
                while (!is_token(ctx, JSH_TOKEN_EOF) &&
                       !(is_token(ctx, JSH_TOKEN_DELIMITER_PUNCT) &&
                         ctx->source_buffer[current(ctx)->start] == ';')) {
                    advance(ctx);
                }
                if (is_token(ctx, JSH_TOKEN_DELIMITER_PUNCT) &&
                    ctx->source_buffer[current(ctx)->start] == ';') {
                    advance(ctx);
                }
                return sue;
            }
            
            if (is_token(ctx, JSH_TOKEN_DELIMITER_BRACKET) &&
                ctx->source_buffer[current(ctx)->start] == '[') {
                while (is_token(ctx, JSH_TOKEN_DELIMITER_BRACKET) &&
                       ctx->source_buffer[current(ctx)->start] == '[') {
                    advance(ctx);
                    while (!is_token(ctx, JSH_TOKEN_EOF) &&
                           !(is_token(ctx, JSH_TOKEN_DELIMITER_BRACKET) &&
                             ctx->source_buffer[current(ctx)->start] == ']')) {
                        advance(ctx);
                    }
                    if (is_token(ctx, JSH_TOKEN_DELIMITER_BRACKET) &&
                        ctx->source_buffer[current(ctx)->start] == ']') {
                        advance(ctx);
                    }
                }
                while (is_token(ctx, JSH_TOKEN_OPERATOR_ARITH) &&
                       ctx->source_buffer[current(ctx)->start] == '*') {
                    advance(ctx);
                }
            }
            
            /* plain typedef: typedef unsigned long my_size;  (also
             * typedef int arr[4]; after the bracket loop above) */
            if (saw_typedef &&
                is_token(ctx, JSH_TOKEN_DELIMITER_PUNCT) &&
                ctx->source_buffer[current(ctx)->start] == ';') {
                JSH_Node *td = jsh_node_create(ctx, JSH_NODE_TYPEDEF_DECLARATION, ctx->file_path);
                td->name.start = name_start;
                td->name.length = name_len;
                td->start_line = name_line;
                advance(ctx);
                return td;
            }
            
            if (is_token(ctx, JSH_TOKEN_DELIMITER_BRACKET) &&
                ctx->source_buffer[current(ctx)->start] == '(') {
                advance(ctx);
                
                int is_func_def = 0;
                if (is_token(ctx, JSH_TOKEN_DELIMITER_BRACKET) &&
                    ctx->source_buffer[current(ctx)->start] == ')') {
                    advance(ctx);
                    if (is_token(ctx, JSH_TOKEN_DELIMITER_PUNCT) &&
                        ctx->source_buffer[current(ctx)->start] == '{') {
                        is_func_def = 1;
                    }
                } else {
                    while (!is_token(ctx, JSH_TOKEN_EOF)) {
                        if (is_token(ctx, JSH_TOKEN_DELIMITER_BRACKET) &&
                            ctx->source_buffer[current(ctx)->start] == ')') {
                            advance(ctx);
                            break;
                        }
                        advance(ctx);
                    }
                    if (is_token(ctx, JSH_TOKEN_DELIMITER_PUNCT) &&
                        ctx->source_buffer[current(ctx)->start] == '{') {
                        is_func_def = 1;
                    }
                }
                
                if (is_func_def) {
                    JSH_Node *func = jsh_node_create(ctx, JSH_NODE_FUNCTION_DEFINITION, ctx->file_path);
                    func->name.start = name_start;
                    func->name.length = name_len;
                    func->start_line = name_line;   /* ADR-0053: the name's line, not the '{' */
                    func->start_col = current(ctx)->col;
                    func->child_capacity = 1;
                    func->children = (JSH_Node **)malloc(sizeof(JSH_Node *));
                    func->children[0] = jsh_parse_compound(ctx);
                    if (func->children[0]) func->child_count = 1;
                    return func;
                }
                
                if (is_token(ctx, JSH_TOKEN_DELIMITER_PUNCT) &&
                    ctx->source_buffer[current(ctx)->start] == ';') {
                    JSH_Node *decl = jsh_node_create(ctx,
                        saw_typedef ? JSH_NODE_TYPEDEF_DECLARATION : JSH_NODE_FUNCTION_DECLARATION,
                        ctx->file_path);
                    decl->name.start = name_start;
                    decl->name.length = name_len;
                    decl->start_line = name_line;
                    advance(ctx);
                    return decl;
                }
                
                if (is_token(ctx, JSH_TOKEN_OPERATOR_ASSIGN)) {
                    advance(ctx);
                    jsh_parse_expression(ctx, 0);
                }
                
                if (is_token(ctx, JSH_TOKEN_DELIMITER_PUNCT) &&
                    ctx->source_buffer[current(ctx)->start] == ';') {
                    advance(ctx);
                }
                
                JSH_Node *decl = jsh_node_create(ctx,
                    saw_typedef ? JSH_NODE_TYPEDEF_DECLARATION : JSH_NODE_VAR_DECLARATION,
                    ctx->file_path);
                decl->name.start = name_start;
                decl->name.length = name_len;
                decl->start_line = name_line;
                return decl;
            }
        }
    }
    
    if (consume_punct(ctx, '{')) {
        JSH_Node *stmt = jsh_node_create(ctx, JSH_NODE_COMPOUND_STMT, ctx->file_path);
        stmt->child_capacity = 4;
        stmt->children = (JSH_Node **)malloc(4 * sizeof(JSH_Node *));
        
        while (!is_token(ctx, JSH_TOKEN_EOF) &&
               !(is_token(ctx, JSH_TOKEN_DELIMITER_PUNCT) &&
                 ctx->source_buffer[current(ctx)->start] == '}')) {
            JSH_Node *child = jsh_parse_statement(ctx);
            if (child) {
                if (stmt->child_count < stmt->child_capacity) {
                    stmt->children[stmt->child_count++] = child;
                }
            }
        }
        
        consume_punct(ctx, '}');
        return stmt;
    }
    
    if (is_token(ctx, JSH_TOKEN_KEYWORD_VALUE) || is_token(ctx, JSH_TOKEN_KEYWORD_CONTROL)) {
        if (check_name(ctx, "return")) {
            JSH_Node *r = jsh_node_create(ctx, JSH_NODE_RETURN_STMT, ctx->file_path);
            advance(ctx);
            r->child_capacity = 1;
            r->children = (JSH_Node **)malloc(sizeof(JSH_Node *));
            r->children[0] = jsh_parse_expression(ctx, 0);
            r->child_count = 1;
            if (is_token(ctx, JSH_TOKEN_DELIMITER_PUNCT) &&
                ctx->source_buffer[current(ctx)->start] == ';') {
                advance(ctx);
            }
            return r;
        }
        
        if (check_name(ctx, "if")) {
            JSH_Node *stmt = jsh_node_create(ctx, JSH_NODE_IF_STMT, ctx->file_path);
            advance(ctx);
            while (is_token(ctx, JSH_TOKEN_DELIMITER_BRACKET) &&
                   ctx->source_buffer[current(ctx)->start] == '(') {
                advance(ctx);
            }
            stmt->child_capacity = 3;
            stmt->children = (JSH_Node **)malloc(3 * sizeof(JSH_Node *));
            stmt->children[0] = jsh_parse_expression(ctx, 0);
            stmt->child_count = 1;
            while (is_token(ctx, JSH_TOKEN_DELIMITER_BRACKET) &&
                   ctx->source_buffer[current(ctx)->start] == ')') {
                advance(ctx);
            }
            stmt->children[1] = jsh_parse_statement(ctx);
            stmt->child_count = 2;
            
            if (is_token(ctx, JSH_TOKEN_KEYWORD_CONTROL) && check_name(ctx, "else")) {
                advance(ctx);
                stmt->children[2] = jsh_parse_statement(ctx);
                stmt->child_count = 3;
            }
            return stmt;
        }
        
        if (check_name(ctx, "while")) {
            JSH_Node *stmt = jsh_node_create(ctx, JSH_NODE_WHILE_STMT, ctx->file_path);
            advance(ctx);
            while (is_token(ctx, JSH_TOKEN_DELIMITER_BRACKET) &&
                   ctx->source_buffer[current(ctx)->start] == '(') {
                advance(ctx);
            }
            stmt->child_capacity = 2;
            stmt->children = (JSH_Node **)malloc(2 * sizeof(JSH_Node *));
            stmt->children[0] = jsh_parse_expression(ctx, 0);
            stmt->child_count = 1;
            while (is_token(ctx, JSH_TOKEN_DELIMITER_BRACKET) &&
                   ctx->source_buffer[current(ctx)->start] == ')') {
                advance(ctx);
            }
            stmt->children[1] = jsh_parse_statement(ctx);
            stmt->child_count = 2;
            return stmt;
        }
        
        if (check_name(ctx, "for")) {
            JSH_Node *stmt = jsh_node_create(ctx, JSH_NODE_FOR_STMT, ctx->file_path);
            advance(ctx);
            while (is_token(ctx, JSH_TOKEN_DELIMITER_BRACKET) &&
                   ctx->source_buffer[current(ctx)->start] == '(') {
                advance(ctx);
            }
            stmt->child_capacity = 4;
            stmt->children = (JSH_Node **)malloc(4 * sizeof(JSH_Node *));
            
            if (!(is_token(ctx, JSH_TOKEN_DELIMITER_PUNCT) &&
                ctx->source_buffer[current(ctx)->start] == ';')) {
                if (is_token(ctx, JSH_TOKEN_KEYWORD_STORAGE) || is_token(ctx, JSH_TOKEN_KEYWORD_TYPE)) {
                    stmt->children[0] = jsh_parse_statement(ctx);
                    stmt->child_count = 1;
                    while (is_token(ctx, JSH_TOKEN_DELIMITER_PUNCT) &&
                           ctx->source_buffer[current(ctx)->start] == ';') {
                        advance(ctx);
                    }
                } else {
                    stmt->children[0] = jsh_parse_expression(ctx, 0);
                    stmt->child_count = 1;
                    while (is_token(ctx, JSH_TOKEN_DELIMITER_PUNCT) &&
                           ctx->source_buffer[current(ctx)->start] == ';') {
                        advance(ctx);
                    }
                }
            } else {
                advance(ctx);
            }
            while (is_token(ctx, JSH_TOKEN_DELIMITER_PUNCT) &&
                   ctx->source_buffer[current(ctx)->start] == ';') {
                advance(ctx);
            }
            if (!(is_token(ctx, JSH_TOKEN_DELIMITER_BRACKET) &&
                ctx->source_buffer[current(ctx)->start] == ')')) {
                stmt->children[1] = jsh_parse_expression(ctx, 0);
                stmt->child_count = 2;
            }
            while (is_token(ctx, JSH_TOKEN_DELIMITER_PUNCT) &&
                   ctx->source_buffer[current(ctx)->start] == ';') {
                advance(ctx);
            }
            if (!(is_token(ctx, JSH_TOKEN_DELIMITER_BRACKET) &&
                ctx->source_buffer[current(ctx)->start] == ')')) {
                stmt->children[2] = jsh_parse_expression(ctx, 0);
                stmt->child_count = 3;
            }
            while (is_token(ctx, JSH_TOKEN_DELIMITER_BRACKET) &&
                   ctx->source_buffer[current(ctx)->start] == ')') {
                advance(ctx);
            }
            while (is_token(ctx, JSH_TOKEN_DELIMITER_BRACKET) &&
                   ctx->source_buffer[current(ctx)->start] == ')') {
                advance(ctx);
            }
            if (is_token(ctx, JSH_TOKEN_DELIMITER_PUNCT) &&
                ctx->source_buffer[current(ctx)->start] == '{') {
                advance(ctx);
                while (!is_token(ctx, JSH_TOKEN_EOF) &&
                       !(is_token(ctx, JSH_TOKEN_DELIMITER_PUNCT) &&
                         ctx->source_buffer[current(ctx)->start] == '}')) {
                    advance(ctx);
                }
                if (is_token(ctx, JSH_TOKEN_DELIMITER_PUNCT) &&
                    ctx->source_buffer[current(ctx)->start] == '}') {
                    advance(ctx);
                }
            } else {
                stmt->children[3] = jsh_parse_statement(ctx);
            }
            stmt->child_count = 4;
            return stmt;
        }
    }
    
    if (consume_punct(ctx, '{')) {
        JSH_Node *stmt = jsh_node_create(ctx, JSH_NODE_COMPOUND_STMT, ctx->file_path);
        stmt->child_capacity = 4;
        stmt->children = (JSH_Node **)malloc(4 * sizeof(JSH_Node *));
        
        while (!is_token(ctx, JSH_TOKEN_EOF) &&
               !(is_token(ctx, JSH_TOKEN_DELIMITER_PUNCT) &&
                 ctx->source_buffer[current(ctx)->start] == '}')) {
            JSH_Node *child = jsh_parse_statement(ctx);
            if (child) {
                if (stmt->child_count < stmt->child_capacity) {
                    stmt->children[stmt->child_count++] = child;
                }
            }
        }
        
        consume_punct(ctx, '}');
        return stmt;
    }
    
    JSH_Node *expr = jsh_parse_expression(ctx, 0);
    if (is_token(ctx, JSH_TOKEN_DELIMITER_PUNCT) &&
        ctx->source_buffer[current(ctx)->start] == ';') {
        advance(ctx);
    } else if (!expr) {
        /* ADR-0053: error recovery must NOT eat '}' — the old loop ran even
         * after consuming ';' above and swallowed the compound's close brace
         * (a bare `foo();` statement made the NEXT function vanish). Stop at
         * '}' without consuming it so the compound keeps its structure. */
        while (!is_token(ctx, JSH_TOKEN_EOF) &&
               !(is_token(ctx, JSH_TOKEN_DELIMITER_PUNCT) &&
                 (ctx->source_buffer[current(ctx)->start] == ';' ||
                  ctx->source_buffer[current(ctx)->start] == '}'))) {
            advance(ctx);
        }
        if (is_token(ctx, JSH_TOKEN_DELIMITER_PUNCT) &&
            ctx->source_buffer[current(ctx)->start] == ';') {
            advance(ctx);
        }
    }
    
    JSH_Node *stmt = jsh_node_create(ctx, JSH_NODE_EXPRESSION_STMT, ctx->file_path);
    if (expr) {
        stmt->child_capacity = 1;
        stmt->children = (JSH_Node **)malloc(sizeof(JSH_Node *));
        stmt->children[0] = expr;
        stmt->child_count = 1;
    }
    return stmt;
}

static void recover_token(JSH_ParseContext *ctx) {
    JSH_Token *t = current(ctx);
    if (t->type == JSH_TOKEN_OPERATOR_ARITH) {
        advance(ctx);
        return;
    }
    if (t->type == JSH_TOKEN_OPERATOR_BITWISE) {
        advance(ctx);
        return;
    }
    if (t->type == JSH_TOKEN_OPERATOR_COMPARE) {
        advance(ctx);
        return;
    }
    if (t->type == JSH_TOKEN_OPERATOR_LOGIC) {
        advance(ctx);
        return;
    }
    if (t->type == JSH_TOKEN_OPERATOR_ASSIGN) {
        advance(ctx);
        return;
    }
    if (tok_idx < ctx->token_count) {
        advance(ctx);
    }
}

JSH_Node *jsh_parse_translation_unit(JSH_ParseContext *ctx) {
    JSH_Node *tu = jsh_node_create(ctx, JSH_NODE_TRANSLATION_UNIT, ctx->file_path);
    tok_idx = 0;
    
    while (!is_token(ctx, JSH_TOKEN_EOF)) {
        uint32_t before = tok_idx;
        JSH_Node *decl = jsh_parse_statement(ctx);
        if (decl) {
            jsh_node_add_child(ctx, tu, decl);
        }
        if (is_token(ctx, JSH_TOKEN_EOF)) break;
        
        if (tok_idx == before) {
            recover_token(ctx);
        }
        
        if (is_token(ctx, JSH_TOKEN_DELIMITER_PUNCT) &&
            ctx->source_buffer[current(ctx)->start] == ';') {
            advance(ctx);
        }
    }
    
    return tu;
}

JSH_Node *jsh_parse_declaration(JSH_ParseContext *ctx) {
    return jsh_parse_statement(ctx);
}

JSH_Node *jsh_parse_function(JSH_ParseContext *ctx, int is_def) {
    return jsh_parse_statement(ctx);
}

JSH_Node *jsh_parse_compound(JSH_ParseContext *ctx) {
    if (!consume_punct(ctx, '{')) {
        return jsh_node_create(ctx, JSH_NODE_COMPOUND_STMT, ctx->file_path);
    }
    
    JSH_Node *compound = jsh_node_create(ctx, JSH_NODE_COMPOUND_STMT, ctx->file_path);
    
    while (!is_token(ctx, JSH_TOKEN_EOF) &&
           !(is_token(ctx, JSH_TOKEN_DELIMITER_PUNCT) &&
             ctx->source_buffer[current(ctx)->start] == '}')) {
        uint32_t before = tok_idx;
        JSH_Node *stmt = jsh_parse_statement(ctx);
        if (stmt) {
            jsh_node_add_child(ctx, compound, stmt);
        }
        if (is_token(ctx, JSH_TOKEN_EOF)) break;
        
        if (tok_idx == before && stmt) {
            recover_token(ctx);
        }
    }
    
    consume_punct(ctx, '}');
    
    return compound;
}

JSH_Node *jsh_parse_binary_expr(JSH_ParseContext *ctx, int min_prec) {
    return jsh_parse_expression(ctx, min_prec);
}

__attribute__((used)) JSH_Node *jsh_parse_primary(JSH_ParseContext *ctx) {
    return parse_primary(ctx);
}

__attribute__((used)) JSH_Node *jsh_parse_unary(JSH_ParseContext *ctx) {
    return parse_primary(ctx);
}

__attribute__((used)) JSH_Node *jsh_parse_if_stmt(JSH_ParseContext *ctx) {
    return jsh_parse_statement(ctx);
}

__attribute__((used)) JSH_Node *jsh_parse_while_stmt(JSH_ParseContext *ctx) {
    return jsh_parse_statement(ctx);
}

__attribute__((used)) JSH_Node *jsh_parse_for_stmt(JSH_ParseContext *ctx) {
    return jsh_parse_statement(ctx);
}

JSH_Node *jsh_parse_struct_union_enum(JSH_ParseContext *ctx, JSH_NodeType node_type) {
    (void)node_type;
    return jsh_parse_statement(ctx);
}

int jsh_parse_init(JSH_ParseContext *ctx, JGGraph *graph, const char *file_path) {
    if (!ctx) return -1;
    
    memset(ctx, 0, sizeof(JSH_ParseContext));
    ctx->graph = graph;
    
    if (file_path) {
        ctx->file_path = strdup(file_path);
        if (!ctx->file_path) return -1;
    }
    
    ctx->tokens = (JSH_Token *)calloc(JSH_MAX_TOKENS, sizeof(JSH_Token));
    if (!ctx->tokens) { jsh_parse_destroy(ctx); return -1; }
    ctx->token_capacity = JSH_MAX_TOKENS;
    
    ctx->nodes = (JSH_Node *)calloc(JSH_MAX_NODES, sizeof(JSH_Node));
    if (!ctx->nodes) {
        jsh_parse_destroy(ctx);
        return -1;
    }
    ctx->node_capacity = JSH_MAX_NODES;
    
    ctx->symbols = (JSH_SymbolEntry *)calloc(1024, sizeof(JSH_SymbolEntry));
    if (!ctx->symbols) { jsh_parse_destroy(ctx); return -1; }
    ctx->symbol_capacity = 1024;
    ctx->symbols[0].hash = 0xFFFFFFFF;
    
    return 0;
}

int jsh_parse_init_with_source(JSH_ParseContext *ctx, JGGraph *graph,
                                const char *source, size_t len, const char *file_path) {
    if (!ctx) return -1;
    
    int rc = jsh_parse_init(ctx, graph, file_path);
    if (rc != 0) return rc;
    
    if (source && len > 0) {
        ctx->source_buffer = (char *)malloc(len + 1);
        if (!ctx->source_buffer) {
            jsh_parse_destroy(ctx);
            return -1;
        }
        memcpy(ctx->source_buffer, source, len);
        ctx->source_buffer[len] = '\0';
        ctx->source_len = len;
    }
    
    return 0;
}

void jsh_parse_destroy(JSH_ParseContext *ctx) {
    if (!ctx) return;
    
    if (ctx->tokens) free(ctx->tokens);
    if (ctx->nodes) {
        for (uint32_t i = 0; i < ctx->node_count; i++) {
            if (ctx->nodes[i].children) free(ctx->nodes[i].children);
            if (ctx->nodes[i].dependencies) free(ctx->nodes[i].dependencies);
        }
        free(ctx->nodes);
    }
    if (ctx->symbols) free(ctx->symbols);
    if (ctx->source_buffer) free(ctx->source_buffer);
    if (ctx->file_path) free((void*)ctx->file_path);
    if (ctx->includes) {
        for (uint32_t i = 0; i < ctx->include_count; i++) {
            if (ctx->includes[i]) free((void*)ctx->includes[i]);
        }
        free(ctx->includes);
    }
    if (ctx->comments) free((void*)ctx->comments);
    
    memset(ctx, 0, sizeof(JSH_ParseContext));
}

int jsh_parse_string(JSH_ParseContext *ctx, const char *src, size_t len) {
    JGGraph *saved_graph = ctx->graph;
    char *saved_file = ctx->file_path ? strdup(ctx->file_path) : NULL;
    if (ctx->file_path && !saved_file) return -1;
    /* One lifecycle path also releases file_path/includes/comments on reparse. */
    jsh_parse_destroy(ctx);
    int rc = jsh_parse_init_with_source(ctx, saved_graph, src, len, saved_file);
    free(saved_file);
    if (rc != 0) return rc;
    
    int result = 0;
    if (jsh_tokenize_all(ctx) != 0) return -1;
    JSH_Node *tu = jsh_parse_translation_unit(ctx);
    
    if (tu && ctx->graph) {
        jsh_ast_to_graph(ctx, ctx->graph);
    }
    
    result = (tu && ctx->had_error == 0) ? 0 : -1;
    return result;
}

int jsh_parse_file(JSH_ParseContext *ctx, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    if (sz < 0) { fclose(f); return -1; }
    char *src = malloc((size_t)sz + 1);
    if (!src) { fclose(f); return -1; }
    /* Terminate at what was actually READ: a short read otherwise left the
     * tail of the buffer uninitialised and handed it to the parser. */
    size_t got = fread(src, 1, (size_t)sz, f);
    src[got] = '\0';
    fclose(f);
    
    int result = jsh_parse_string(ctx, src, (long)got);
    free(src);
    return result;
}

int jsh_ast_to_graph(JSH_ParseContext *ctx, JGGraph *graph) {
    if (!ctx || !graph || !graph->initialized) return -1;
    
    if (!ctx->nodes || ctx->node_count == 0) return 0;
    
    for (uint32_t i = 0; i < ctx->node_count; i++) {
        JSH_Node *node = &ctx->nodes[i];
        if (!node) continue;
        
        JGNodeType gtype = JG_NODE_VARIABLE;
        if (node->type == JSH_NODE_FUNCTION_DEFINITION || 
            node->type == JSH_NODE_FUNCTION_DECLARATION) {
            gtype = JG_NODE_FUNCTION;
        } else if (node->type == JSH_NODE_STRUCT_DECLARATION ||
                   node->type == JSH_NODE_UNION_DECLARATION) {
            gtype = JG_NODE_STRUCT;
        } else if (node->type == JSH_NODE_ENUM_DECLARATION) {
            gtype = JG_NODE_ENUM;
        } else if (node->type == JSH_NODE_VAR_DECLARATION) {
            gtype = JG_NODE_VARIABLE;
        }
        
        char name[256] = "";
        if (node->name.length > 0 && node->name.start < ctx->source_len) {
            size_t nl = node->name.length;
            if (nl > 255) nl = 255;
            if (node->name.start + nl <= ctx->source_len) {
                memcpy(name, &ctx->source_buffer[node->name.start], nl);
                name[nl] = '\0';
            }
        }
        
        uint32_t graph_idx = jg_add_node(graph, name, node->filename,
                                         node->start_line, node->start_col, gtype);
        node->symbol_id = graph_idx;
        
        for (uint32_t c = 0; c < node->child_count && node->children; c++) {
            JSH_Node *child = node->children[c];
            if (!child) continue;
            
            uint32_t child_idx = 0;
            if (jsh_node_to_graph_node(ctx, child, &child_idx) == 0) {
                jg_add_edge(graph, child_idx, graph_idx, JG_REL_CONTAINS, JG_CONF_EXTRACTED);
            }
        }
    }
    
    graph->dirty = 1;
    graph->parse_version++;
    return 0;
}

int jsh_node_to_graph_node(JSH_ParseContext *ctx, JSH_Node *ast, uint32_t *graph_idx) {
    if (!ast || !ctx || !graph_idx) return -1;
    
    if (ast->symbol_id != 0) {
        *graph_idx = ast->symbol_id;
        return 0;
    }
    
    JGNodeType node_type = JG_NODE_VARIABLE;
    if (ast->type == JSH_NODE_FUNCTION_DEFINITION || 
        ast->type == JSH_NODE_FUNCTION_DECLARATION) {
        node_type = JG_NODE_FUNCTION;
    } else if (ast->type == JSH_NODE_STRUCT_DECLARATION ||
               ast->type == JSH_NODE_UNION_DECLARATION) {
        node_type = JG_NODE_STRUCT;
    } else if (ast->type == JSH_NODE_ENUM_DECLARATION) {
        node_type = JG_NODE_ENUM;
    }
    
    char name[256] = "";
    if (ast->name.length > 0 && ast->name.start < ctx->source_len) {
        size_t nl = ast->name.length;
        if (nl > 255) nl = 255;
        if (ast->name.start + nl <= ctx->source_len) {
            memcpy(name, &ctx->source_buffer[ast->name.start], nl);
            name[nl] = '\0';
        }
    }
    
    *graph_idx = jg_add_node(ctx->graph, name, ast->filename,
                             ast->start_line, ast->start_col, node_type);
    ast->symbol_id = *graph_idx;
    return 0;
}
