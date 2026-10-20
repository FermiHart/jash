/* Direct lexer/AST tests: the native code parser is a graph extractor, not a
 * `jash parse` command and not a validating C compiler. */
#include "graph/jash_graph.h"
#include "graph/jash_parse.h"
#include <stdio.h>
#include <string.h>

static int failures;
static unsigned tests;
static void check(int ok, const char* name) {
    tests++;
    if (!ok) { fprintf(stderr, "FAIL: %s\n", name); failures++; }
}

static void ast(const char* source, const char* name, JSH_NodeType type) {
    JSH_ParseContext ctx = {0};
    int rc = jsh_parse_init_with_source(&ctx, NULL, source, strlen(source), "fixture.c");
    check(rc == 0, "allocate parser");
    if (rc != 0) return;
    rc = jsh_tokenize_all(&ctx);
    JSH_Node* root = rc == 0 ? jsh_parse_translation_unit(&ctx) : NULL;
    int found = 0;
    for (unsigned i = 0; i < ctx.node_count; i++) {
        JSH_Node* n = &ctx.nodes[i];
        if (n->type == type && n->name.length == strlen(name) &&
            !memcmp(ctx.source_buffer + n->name.start, name, n->name.length)) found++;
    }
    check(root && root->child_count > 0 && found == 1, name);
    jsh_parse_destroy(&ctx);
}

static void lexical(const char* source, int valid) {
    JSH_ParseContext ctx = {0};
    if (jsh_parse_init_with_source(&ctx, NULL, source, strlen(source), "fixture.c") != 0) {
        check(0, "allocate lexer"); return;
    }
    int rc = jsh_tokenize_all(&ctx);
    check(valid ? rc == 0 : rc != 0 && ctx.had_error, source);
    check(ctx.current_token <= ctx.source_len, "lexer stays within source");
    check(ctx.token_count > 0 && ctx.tokens[ctx.token_count - 1].type == JSH_TOKEN_EOF,
          "lexer terminates with EOF even on error");
    jsh_parse_destroy(&ctx);
}

int main(void) {
    ast("int add(int a, int b) { return a+b; }", "add", JSH_NODE_FUNCTION_DEFINITION);
    ast("static int helper(void) { return 42; }", "helper", JSH_NODE_FUNCTION_DEFINITION);
    ast("int proto(void);", "proto", JSH_NODE_FUNCTION_DECLARATION);
    ast("struct Point { int x; int y; };", "Point", JSH_NODE_STRUCT_DECLARATION);
    ast("enum Color { Red, Blue };", "Color", JSH_NODE_ENUM_DECLARATION);
    ast("typedef unsigned long Size;", "Size", JSH_NODE_TYPEDEF_DECLARATION);
    ast("/* int fake(){} */\nint real(void) { return 1; }", "real", JSH_NODE_FUNCTION_DEFINITION);
    ast("#define MACRO(x) \\\n        ((x)+1)\nint after(void) { return MACRO(1); }", "after", JSH_NODE_FUNCTION_DEFINITION);
    lexical("\"escaped \\\" quote\"", 1);
    lexical("'\\n'", 1);
    lexical("", 1);
    lexical("\"unterminated", 0);
    lexical("\"trailing\\", 0);
    lexical("'unterminated", 0);
    lexical("'\\", 0);
    lexical("/* unterminated", 0);
    lexical("@", 0);
    JSH_ParseContext ctx = {0};
    const char* source = "a b c d e";
    if (jsh_parse_init_with_source(&ctx, NULL, source, strlen(source), "cap.c") == 0) {
        ctx.token_capacity = 3;
        check(jsh_tokenize_all(&ctx) != 0 && ctx.had_error, "token cap fails visibly");
        check(ctx.token_count <= ctx.token_capacity, "EOF fits token capacity");
        jsh_parse_destroy(&ctx);
    } else check(0, "allocate cap test");
    memset(&ctx, 0, sizeof(ctx));
    check(jsh_parse_init(&ctx, NULL, "reparse.c") == 0, "init repeated parse");
    for (int i = 0; i < 3; i++)
        check(jsh_parse_string(&ctx, "int f(){return 1;}", 18) == 0, "repeated parse");
    jsh_parse_destroy(&ctx);
    printf("native parser: %u checks, %d failures\n", tests, failures);
    return failures ? 1 : 0;
}
