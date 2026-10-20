// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// ADR-0046: Jash Native Code Parser - Contextual AST Generation
//
// This parser is NOT a generic tree-sitter clone. It is a domain-aware
// parser that learns from the codebase's own patterns, history, and
// conventions. It generates contextual AST with embedded project knowledge.
//
// Design Principles:
// 1. Security-first: No buffer overflows, strict bounds checking
// 2. Memory-safe: Arena-based allocation, no leaks
// 3. Context-aware: Learns patterns from project history
// 4. Incremental: Only reparses changed files
// 5. Zero-dependency: Pure C11, no external libs

#ifndef JASH_PARSE_H
#define JASH_PARSE_H

#include <stdint.h>
#include <stddef.h>
#include <sys/stat.h>

#define JSH_MAX_TOKENS     (1 << 17)    // 128k tokens per file (ADR-0053: eval.c-class files)
#define JSH_MAX_NODES      (1 << 18)    // 256k AST nodes
#define JSH_MAX_NODES_PER_DECL (1024)

#define JSH_TOKEN_NAME_LEN   64
#define JSH_TOKEN_SRC_LEN    1024
#define JSH_MAX_COMMENTS     1024
#define JSH_MAX_INCLUDES     256
#define JSH_MAX_DEPS         4096

typedef enum {
    JSH_TOKEN_EOF = 0,
    JSH_TOKEN_NEWLINE,
    JSH_TOKEN_COMMENT_LINE,
    JSH_TOKEN_COMMENT_BLOCK,
    JSH_TOKEN_STRING_LITERAL,
    JSH_TOKEN_CHAR_LITERAL,
    JSH_TOKEN_NUMBER_INT,
    JSH_TOKEN_NUMBER_FLOAT,
    JSH_TOKEN_HEX_LITERAL,
    JSH_TOKEN_BINARY_LITERAL,
    JSH_TOKEN_OCTAL_LITERAL,
    JSH_TOKEN_OPERATOR_ARITH,     // + - * / %
    JSH_TOKEN_OPERATOR_BITWISE,   // & | ^ ~ << >>
    JSH_TOKEN_OPERATOR_COMPARE,   // == != < > <= >=
    JSH_TOKEN_OPERATOR_LOGIC,     // && || !
    JSH_TOKEN_OPERATOR_ASSIGN,    // = += -= *= /= %=
    JSH_TOKEN_DELIMITER_PUNCT,    // ; , . : ? 
    JSH_TOKEN_DELIMITER_BRACKET,  // ( ) { } [ ]
    JSH_TOKEN_PREPROCESSOR,       // #define, #include, #ifdef, etc.
    JSH_TOKEN_KEYWORD_STORAGE,    // static, extern, typedef, const, etc.
    JSH_TOKEN_KEYWORD_CONTROL,    // if, else, for, while, do, switch, etc.
    JSH_TOKEN_KEYWORD_VALUE,      // return, goto, break, continue, case, default
    JSH_TOKEN_KEYWORD_TYPE,       // void, int, char, float, double, struct, union, enum
    JSH_TOKEN_KEYWORD_OTHER,      // sizeof, alignof (C11)
    JSH_TOKEN_IDENTIFIER,
    JSH_TOKEN_STRING_TEMPLATE,    // C++ raw strings, custom template strings
    JSH_TOKEN_USER_LITERAL,       // user-defined literals
} JSH_TokenType;

typedef struct {
    JSH_TokenType type;
    uint32_t start;
    uint32_t length;
    uint32_t line;
    uint32_t col;
    uint32_t hash;           // For O(1) identifier lookup
    uint16_t prefix_flags;   // JSH_TOKEN_PREFIX_*
    uint16_t suffix_flags;
} JSH_Token;

#define JSH_TOKEN_PREFIX_NONE     0x0000
#define JSH_TOKEN_PREFIX_CONST      0x0001
#define JSH_TOKEN_PREFIX_VOLATILE   0x0002
#define JSH_TOKEN_PREFIX_INLINE     0x0004

typedef enum {
    JSH_NODE_PROGRAM = 0,
    JSH_NODE_TRANSLATION_UNIT,
    JSH_NODE_FUNCTION_DEFINITION,
    JSH_NODE_FUNCTION_DECLARATION,
    JSH_NODE_FUNCTION_CALL,
    JSH_NODE_PARAM_LIST,
    JSH_NODE_COMPOUND_STMT,
    JSH_NODE_BLOCK,
    JSH_NODE_EXPRESSION_STMT,
    JSH_NODE_RETURN_STMT,
    JSH_NODE_IF_STMT,
    JSH_NODE_WHILE_STMT,
    JSH_NODE_DO_WHILE_STMT,
    JSH_NODE_FOR_STMT,
    JSH_NODE_SWITCH_STMT,
    JSH_NODE_CASE_STMT,
    JSH_NODE_DEFAULT_STMT,
    JSH_NODE_BREAK_STMT,
    JSH_NODE_CONTINUE_STMT,
    JSH_NODE_GOTO_STMT,
    JSH_NODE_LABEL_STMT,
    JSH_NODE_DECLARATION,
    JSH_NODE_TYPE_DECLARATION,
    JSH_NODE_VAR_DECLARATION,
    JSH_NODE_TYPEDEF_DECLARATION,
    JSH_NODE_STRUCT_DECLARATION,
    JSH_NODE_ENUM_DECLARATION,
    JSH_NODE_UNION_DECLARATION,
    JSH_NODE_INITIALIZER,
    JSH_NODE_ASSIGNMENT_EXPR,
    JSH_NODE_BINARY_EXPR,
    JSH_NODE_UNARY_EXPR,
    JSH_NODE_CAST_EXPR,
JSH_NODE_CONDITION_EXPR,
    JSH_NODE_COMMA_EXPR,
    JSH_NODE_SIZEOF_EXPR,
    JSH_NODE_SELECT_EXPR,
    JSH_NODE_CONDITIONAL_EXPR,
    JSH_NODE_FUNC_UNKNOWN,      // Placeholder for error recovery
} JSH_NodeType;

// Symbol table entry (forward declaration)
typedef struct {
    char name[64];
    uint32_t node_idx;
    uint32_t hash;
} JSH_SymbolEntry;

typedef struct JSH_Node JSH_Node;

struct JSH_Node {
    JSH_NodeType type;
    JSH_Token name;
    JSH_Token decl_keyword;
    JSH_Token type_keyword;
    JSH_Token return_type;
    uint32_t start_byte;
    uint32_t end_byte;
    uint32_t start_line;
    uint32_t end_line;
    uint32_t start_col;
    uint32_t end_col;
    uint32_t child_count;
    uint32_t child_capacity;
    JSH_Node **children;
    const char *filename;
    uint64_t mtime_ns;
    uint32_t dependency_count;
    uint32_t dependency_capacity;
    char **dependencies;
    uint32_t symbol_id;
    uint32_t scope_depth;
    uint8_t is_definition;
    uint8_t is_static;
    uint8_t is_extern;
    uint8_t is_const;
    uint8_t is_volatile;
    uint8_t has_body;
    char name_buffer[256];  // Cached name for graph output
};

typedef struct {
    // Token storage
    char *source_buffer;
    size_t source_capacity;
    size_t source_len;
    JSH_Token *tokens;
    uint32_t token_count;
    uint32_t token_capacity;
    
    // AST storage
    JSH_Node *nodes;
    uint32_t node_count;
    uint32_t node_capacity;
    
    // Symbol table (perfect hash for identifiers)
    uint32_t symbol_count;
    uint32_t symbol_capacity;
    JSH_SymbolEntry *symbols;
    
    // File registry
    char *file_path;
    struct stat file_stat;
    uint64_t mtime_ns;            // File modification time (nanoseconds)
    uint32_t *inode_map;       // For hardlink detection
    char **includes;           // Direct includes
    uint32_t include_count;
    uint32_t include_capacity;
    
    // Dependencies
    uint32_t *deps_forward;    // What this file depends on
    uint32_t *deps_reverse;    // What depends on this file
    uint32_t dep_count;
    
    // Comments (preserved for documentation generation)
    struct {
        uint32_t start;
        uint32_t length;
        uint32_t line;
    } *comments;
    uint32_t comment_count;
    
    // Parser state
    uint32_t current_token;
    uint32_t current_node;
    int had_error;
    char error_msg[256];
    
    // Graph context
    JGGraph *graph;            // Link to main graph
    
    // Configuration
    int dialect_flags;         // JSH_DIALECT_*
    int strict_mode;
    int incremental_mode;
} JSH_ParseContext;

// Parser configuration flags
#define JSH_DIALECT_C89        0x0001
#define JSH_DIALECT_C99        0x0002
#define JSH_DIALECT_C11        0x0004
#define JSH_DIALECT_C17        0x0008
#define JSH_DIALECT_CPP98      0x0010
#define JSH_DIALECT_CPP11      0x0020
#define JSH_DIALECT_CPP14      0x0040
#define JSH_DIALECT_CPP17      0x0080
#define JSH_DIALECT_CPP20      0x0100

// Parser functions
int jsh_parse_init(JSH_ParseContext *ctx, JGGraph *graph, const char *file_path);
int jsh_parse_init_with_source(JSH_ParseContext *ctx, JGGraph *graph, 
                               const char *source, size_t len, const char *file_path);
void jsh_parse_destroy(JSH_ParseContext *ctx);

// Tokenization
JSH_Token jsh_tokenize_next(JSH_ParseContext *ctx);
int jsh_tokenize_all(JSH_ParseContext *ctx);
uint32_t jsh_hash_identifier(const char *s, size_t len);

// Parsing
JSH_Node *jsh_parse_translation_unit(JSH_ParseContext *ctx);
JSH_Node *jsh_parse_declaration(JSH_ParseContext *ctx);
JSH_Node *jsh_parse_function(JSH_ParseContext *ctx, int is_definition);
JSH_Node *jsh_parse_struct_union_enum(JSH_ParseContext *ctx, JSH_NodeType node_type);
JSH_Node *jsh_parse_initializer(JSH_ParseContext *ctx);
JSH_Node *jsh_parse_statement(JSH_ParseContext *ctx);
JSH_Node *jsh_parse_expression(JSH_ParseContext *ctx, int precedence);
JSH_Node *jsh_parse_compound(JSH_ParseContext *ctx);

// Binary expression parser (precedence climbing)
JSH_Node *jsh_parse_binary_expr(JSH_ParseContext *ctx, int min_prec);

// Helper functions
JSH_Node *jsh_node_create(JSH_ParseContext *ctx, JSH_NodeType type, const char *filename);
JSH_Node *jsh_node_add_child(JSH_ParseContext *ctx, JSH_Node *parent, JSH_Node *child);
void jsh_node_set_extent(JSH_ParseContext *ctx, JSH_Node *n, uint32_t start, uint32_t end);

// Symbol resolution
int jsh_symbol_insert(JSH_ParseContext *ctx, const char *name, JSH_Node *node);
JSH_Node *jsh_symbol_lookup(JSH_ParseContext *ctx, const char *name);
uint32_t jsh_symbol_hash(const char *name);

// Include dependency tracking
int jsh_add_include(JSH_ParseContext *ctx, const char *include_path);
int jsh_resolve_includes(JSH_ParseContext *ctx);

// File parsing entry point
int jsh_parse_file(JSH_ParseContext *ctx, const char *path);
int jsh_parse_string(JSH_ParseContext *ctx, const char *src, size_t len);

// AST to Graph conversion
int jsh_ast_to_graph(JSH_ParseContext *ctx, JGGraph *graph);
int jsh_node_to_graph_node(JSH_ParseContext *ctx, JSH_Node *ast, uint32_t *graph_idx);

// Public statement/expression parse entry points
JSH_Node *jsh_parse_statement(JSH_ParseContext *ctx);
JSH_Node *jsh_parse_expression(JSH_ParseContext *ctx, int min_prec);
JSH_Node *jsh_parse_binary_expr(JSH_ParseContext *ctx, int min_prec);
JSH_Node *jsh_parse_primary(JSH_ParseContext *ctx);
JSH_Node *jsh_parse_unary(JSH_ParseContext *ctx);
JSH_Node *jsh_parse_if_stmt(JSH_ParseContext *ctx);
JSH_Node *jsh_parse_while_stmt(JSH_ParseContext *ctx);
JSH_Node *jsh_parse_for_stmt(JSH_ParseContext *ctx);

#endif
