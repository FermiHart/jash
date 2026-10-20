// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — builtins/builtin_prompt_forge — Prompt Optimization Engine (ADR-0034)
// F E R M I ∞ H A R T
//
// Commands:
//   prompt optimize "prompt"     optimize prompt for token savings
//   prompt analyze "prompt"      analyze prompt for redundancy
//   prompt compress "prompt"     compress prompt preserving intent
//   prompt stats                 show optimization statistics

#include <builtins/builtin.h>
#include <theme/io.h>
#include <theme/colors.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <limits.h>
#include "semantic_builtin.h"

#define PROMPT_MAX 8192
#define TOKEN_RATIO 4  // ~4 chars per token

// ── Token Counter (no LLM needed) ───────────────────────────────────────────

static int count_tokens(const char* text) {
    if (!text || !*text) return 0;
    int tokens = 0;
    int in_word = 0;
    const char* p = text;
    while (*p) {
        if (*p == ' ' || *p == '\t' || *p == '\n') {
            if (in_word) { tokens++; in_word = 0; }
        } else {
            in_word = 1;
        }
        p++;
    }
    if (in_word) tokens++;
    return tokens;
}

// ── Verbose Pattern Database ─────────────────────────────────────────────────

typedef struct {
    const char* verbose;
    const char* concise;
    int savings;  // chars saved
} verbose_pattern_t;

static const verbose_pattern_t PATTERNS[] = {
    // ========== AGGRESSIVE: Portuguese → "Indian English" style ==========
    // Goal: "explicar funcionamento git" not "ajude explicar como funciona o git"

    // Full verbose → verb only
    {"por favor me ajude a explicar", "explicar", 22},
    {"por favor, me ajude a explicar", "explicar", 23},
    {"por favor me ajude a criar", "criar", 21},
    {"por favor me ajude a fazer", "fazer", 21},
    {"por favor me ajude a resolver", "resolver", 23},
    {"por favor me ajude a encontrar", "encontrar", 24},
    {"por favor me ajude a entender", "entender", 23},
    {"por favor me ajude a", "", 18},
    {"por favor, me ajude a", "", 19},
    {"me ajude a explicar", "explicar", 15},
    {"me ajude a criar", "criar", 13},
    {"me ajude a fazer", "fazer", 13},
    {"me ajude a resolver", "resolver", 15},
    {"me ajude a encontrar", "encontrar", 16},
    {"me ajude a entender", "entender", 15},
    {"me ajude a", "", 9},
    {"me ajude", "", 8},

    // "como funciona X" → "funcionamento X"
    {"como funciona", "funcionamento", 0},
    {"como funcionam", "funcionamento", 0},

    // "me explique como X" → "explicar X"
    {"me explique como", "explicar", 14},
    {"me explique", "explicar", 11},
    {"me diga como", "dizer", 9},
    {"me diga", "dizer", 7},
    {"me mostre como", "mostrar", 11},
    {"me mostre", "mostrar", 9},
    {"me fale como", "dizer", 9},
    {"me fale", "dizer", 7},
    {"me ensine como", "ensinar", 11},
    {"me ensine", "ensinar", 9},
    {"me descreva como", "descrever", 13},
    {"me descreva", "descrever", 11},
    {"me liste como", "listar", 9},
    {"me liste", "listar", 8},
    {"me compare como", "comparar", 11},
    {"me compare", "comparar", 9},
    {"me recomende como", "recomendar", 14},
    {"me recomende", "recomendar", 12},
    {"me sugira como", "sugerir", 11},
    {"me sugira", "sugerir", 9},

    // "como posso fazer X" → "fazer X"
    {"como posso fazer", "fazer", 7},
    {"como eu posso fazer", "fazer", 11},
    {"como eu faço", "fazer", 8},
    {"como posso", "fazer", 6},

    // "qual é a melhor maneira de X" → "X"
    {"qual é a melhor maneira de", "", 22},
    {"qual é a melhor forma de", "", 20},
    {"existe algum jeito de", "", 17},
    {"existe alguma forma de", "", 18},
    {"gostaria de saber", "quero", 13},
    {"gostaria de saber como", "quero", 19},
    {"pode me dizer", "", 10},
    {"pode me dizer como", "", 15},
    {"é possível", "", 6},
    {"é possível fazer", "", 11},
    {"deixe-me explicar", "", 8},
    {"preciso de ajuda com", "", 12},
    {"preciso de ajuda para", "", 12},
    {"você pode", "", 5},
    {"você poderia", "", 8},
    {"seria possível", "", 10},
    {"seria possível fazer", "", 15},
    {"por favor explique", "explicar", 16},
    {"por favor, explique", "explicar", 17},

    // ========== AGGRESSIVE: English → "Indian English" style ==========
    // Goal: "explain git function" not "can you please help me understand how to explain git function"

    // Full verbose → verb only
    {"can you please help me understand", "explain", 29},
    {"could you please help me understand", "explain", 31},
    {"can you please help me", "help", 19},
    {"could you please help me", "help", 21},
    {"i would like to know how to", "want", 22},
    {"i would like to know", "want", 16},
    {"i was wondering if you could", "can", 22},
    {"would you mind explaining", "explain", 20},
    {"do you think you could", "can", 17},
    {"is it possible to", "can", 12},
    {"what is the best way to", "", 17},
    {"what is the easiest way to", "", 21},
    {"how do i go about", "", 13},
    {"could you tell me how to", "", 19},
    {"can you tell me how to", "", 18},
    {"i need help with", "", 8},
    {"i need assistance with", "", 14},
    {"can you explain to me", "explain", 15},
    {"would you be able to", "can", 14},
    {"i'm looking for", "find", 11},
    {"i'm trying to figure out", "figure out", 15},
    {"what would be the best", "", 17},
    {"how would you suggest", "suggest", 16},
    {"what do you recommend", "", 15},

    // "how to X" → "X"
    {"how to", "", 7},

    // Code patterns → verb + object
    {"write me a function that", "function", 18},
    {"write me a function which", "function", 19},
    {"create a function that", "function", 16},
    {"create a function which", "function", 17},
    {"write a function that", "function", 15},
    {"write a function which", "function", 16},
    {"write me code that", "code", 14},
    {"write code that", "code", 10},
    {"write me a script that", "script", 17},
    {"write a script that", "script", 14},
    {"can you write", "write", 10},
    {"could you write", "write", 12},
    {"would you write", "write", 12},

    // ========== FILLER REMOVAL (aggressive) ==========
    {"please ", "", 7},
    {"por favor ", "", 10},
    {"por favor, ", "", 11},
    {"thanks ", "", 7},
    {"thank you ", "", 10},
    {"obrigado ", "", 9},
    {"obrigada ", "", 9},

    // Articles (Portuguese)
    {" o ", " ", 3},
    {" a ", " ", 3},
    {" os ", " ", 4},
    {" as ", " ", 4},
    {" um ", " ", 4},
    {" uma ", " ", 5},

    // Articles (English)
    {" the ", " ", 5},
    {" a ", " ", 3},
    {" an ", " ", 4},

    // Pronouns
    {" me ", " ", 4},
    {" my ", " ", 4},
    {" i ", " ", 3},

    // Filler words
    {"you ", "", 4},
    {"can you ", "", 8},
    {"could you ", "", 10},
    {"would you ", "", 10},
    {"do you ", "", 7},
    {"i want to ", "", 10},
    {"i need to ", "", 10},
    {"i would like to ", "", 15},

    // Redundant
    {"some ", "", 5},
    {"any ", "", 4},

    {NULL, NULL, 0}
};

// ── Intent Extractor ─────────────────────────────────────────────────────────

static const char* extract_intent(const char* text) {
    // Simple intent detection based on common verbs
    const char* intents[] = {
        "explique", "explain",
        "crie", "create",
        "escreva", "write",
        "corrija", "fix",
        "otimize", "optimize",
        "analise", "analyze",
        "encontre", "find",
        "implemente", "implement",
        "teste", "test",
        "documente", "document",
        "refatore", "refactor",
        "depure", "debug",
        "mostre", "show",
        "liste", "list",
        "compare", "compare",
        "explique", "explain",
        NULL
    };

    for (int i = 0; intents[i] != NULL; i += 2) {
        const char* pt = intents[i];
        const char* en = intents[i+1];

        // Check Portuguese
        if (strstr(text, pt)) return pt;
        // Check English
        if (strstr(text, en)) return en;
    }

    return NULL;
}

// ── Compression Engine ───────────────────────────────────────────────────────

static int compress_prompt(const char* input, char* output, size_t outsz) {
    char temp[PROMPT_MAX];
    if (!input || !output || !outsz || strlen(input) >= sizeof(temp)) return INT_MIN;
    strlcpy(temp, input, sizeof(temp));
    temp[sizeof(temp) - 1] = '\0';

    int total_savings = 0;

    // Apply pattern replacements
    for (int i = 0; PATTERNS[i].verbose != NULL; i++) {
        char* found = strstr(temp, PATTERNS[i].verbose);
        if (found) {
            int verbose_len = strlen(PATTERNS[i].verbose);
            int concise_len = strlen(PATTERNS[i].concise);
            int savings = verbose_len - concise_len;

            // Apply all patterns (even if savings <= 0, they may enable other patterns)
            if (savings > 0 || concise_len > 0) {
                if (strlen(temp) - (size_t)verbose_len + (size_t)concise_len >= sizeof(temp)) return INT_MIN;
                // Shift remaining text
                memmove(found + concise_len, found + verbose_len, strlen(found + verbose_len) + 1);
                memcpy(found, PATTERNS[i].concise, concise_len);
                total_savings += savings;
            }
        }
    }

    // Remove multiple spaces
    char* src = temp;
    char* dst = output;
    int prev_space = 0;

    while (*src && (size_t)(dst - output) < outsz - 1) {
        if (*src == ' ' || *src == '\t') {
            if (!prev_space) {
                *dst++ = ' ';
                prev_space = 1;
            }
        } else {
            *dst++ = *src;
            prev_space = 0;
        }
        src++;
    }
    *dst = '\0';

    // Trim trailing space
    size_t len = strlen(output);
    while (len > 0 && output[len-1] == ' ') {
        output[--len] = '\0';
    }

    return total_savings;
}

static int prompt_semantic(semantic_builtin_t* call, jash_state_t* state, int argc, char** argv) {
    (void)state;
    const char* operation = argc ? argv[0] : "stats";
    int stats = !strcmp(operation, "stats"), analyze = !strcmp(operation, "analyze");
    if ((stats && argc > 1) || (!stats && (argc != 2 || !argv[1][0] || strlen(argv[1]) >= PROMPT_MAX)) ||
        (!stats && !analyze && strcmp(operation, "compress") && strcmp(operation, "optimize")))
        return semantic_error(call, "invalid_arguments_or_prompt_limit", 2);
    semantic_value_t* sem = &call->values;
    jdv_record_input_t fields[16]; size_t count = 0;
    fields[count++] = semantic_field("schema", semantic_symbol(sem, call->schema_id));
    fields[count++] = semantic_field("operation", semantic_symbol(sem, operation));
    fields[count++] = semantic_field("engine", semantic_symbol(sem, "heuristic-table"));
    fields[count++] = semantic_field("token_estimator", semantic_symbol(sem, "whitespace-word-count"));
    fields[count++] = semantic_field("semantic_preservation_verified", jdv_value_bool(0));
    fields[count++] = semantic_field("patterns_loaded", jdv_value_u64(sizeof(PATTERNS) / sizeof(PATTERNS[0]) - 1));
    if (stats) {
        jdv_value_t languages[] = {semantic_symbol(sem, "Portuguese"), semantic_symbol(sem, "English")};
        fields[count++] = semantic_field("languages", semantic_list(sem, languages, 2));
    } else {
        const char* input = argv[1]; char output[PROMPT_MAX];
        jdv_value_t patterns[sizeof(PATTERNS) / sizeof(PATTERNS[0])]; size_t used = 0;
        for (size_t i = 0; PATTERNS[i].verbose; i++) if (strstr(input, PATTERNS[i].verbose)) {
            jdv_record_input_t pattern[] = {
                semantic_field("verbose", semantic_text(sem, PATTERNS[i].verbose)),
                semantic_field("replacement", semantic_text(sem, PATTERNS[i].concise))
            };
            patterns[used++] = semantic_record(sem, pattern, 2);
        }
        fields[count++] = semantic_field("input", semantic_text(sem, input));
        fields[count++] = semantic_field("input_bytes", jdv_value_filesize(strlen(input)));
        fields[count++] = semantic_field("input_tokens_estimated", jdv_value_u64((uint64_t)count_tokens(input)));
        fields[count++] = semantic_field("intent", semantic_text(sem, extract_intent(input)));
        fields[count++] = semantic_field("matched_patterns", semantic_list(sem, patterns, used));
        if (!analyze) {
            if (compress_prompt(input, output, sizeof(output)) == INT_MIN) return semantic_error(call, "compression_output_limit", 75);
            fields[count++] = semantic_field("output", semantic_text(sem, output));
            fields[count++] = semantic_field("output_bytes", jdv_value_filesize(strlen(output)));
            fields[count++] = semantic_field("output_tokens_estimated", jdv_value_u64((uint64_t)count_tokens(output)));
            fields[count++] = semantic_field("savings_bytes", jdv_value_i64((int64_t)strlen(input) - (int64_t)strlen(output)));
        }
    }
    return semantic_finish(call, semantic_record(sem, fields, count), 0);
}

// ── Subcommands ──────────────────────────────────────────────────────────────

static int cmd_optimize(jash_writer_t w, void* ctx, const char* args) {
    if (!args || !*args) {
        jash_writef(w, ctx, "usage: prompt optimize \"prompt\"\n");
        return 1;
    }

    char optimized[PROMPT_MAX];
    int savings = compress_prompt(args, optimized, sizeof(optimized));

    int original_tokens = count_tokens(args);
    int optimized_tokens = count_tokens(optimized);
    int tokens_saved = original_tokens - optimized_tokens;

    jash_writef(w, ctx, "%sPrompt optimization:%s\n", J_ROLE_ACCENT, J_RESET);
    jash_writef(w, ctx, "\n  %sOriginal (%d tokens):%s\n", J_ROLE_OK, original_tokens, J_RESET);
    jash_writef(w, ctx, "    %s%s%s\n", J_ROLE_MUTED, args, J_RESET);
    jash_writef(w, ctx, "\n  %sOptimized (%d tokens):%s\n", J_ROLE_OK, optimized_tokens, J_RESET);
    jash_writef(w, ctx, "    %s%s%s\n", J_ROLE_OK, optimized, J_RESET);

    if (savings > 0) {
        int percent = (savings * 100) / strlen(args);
        jash_writef(w, ctx, "\n  %s✓ Savings: %d chars (%d%%) | %d tokens%s\n",
                    J_ROLE_OK, savings, percent, tokens_saved, J_RESET);
    } else {
        jash_writef(w, ctx, "\n  %s✓ Already optimal%s\n", J_ROLE_OK, J_RESET);
    }

    return 0;
}

static int cmd_analyze(jash_writer_t w, void* ctx, const char* args) {
    if (!args || !*args) {
        jash_writef(w, ctx, "usage: prompt analyze \"prompt\"\n");
        return 1;
    }

    int tokens = count_tokens(args);
    int chars = strlen(args);

    jash_writef(w, ctx, "%sPrompt analysis:%s\n", J_ROLE_ACCENT, J_RESET);
    jash_writef(w, ctx, "  Tokens: %d\n", tokens);
    jash_writef(w, ctx, "  Characters: %d\n", chars);
    jash_writef(w, ctx, "  Avg chars/token: %.1f\n", chars > 0 ? (float)chars/tokens : 0);

    // Detect verbose patterns
    int verbose_count = 0;
    for (int i = 0; PATTERNS[i].verbose != NULL; i++) {
        if (strstr(args, PATTERNS[i].verbose)) {
            verbose_count++;
        }
    }

    if (verbose_count > 0) {
        jash_writef(w, ctx, "\n  %s⚠ Verbose patterns found: %d%s\n", J_ROLE_WARN, verbose_count, J_RESET);
    } else {
        jash_writef(w, ctx, "\n  %s✓ No verbose patterns detected%s\n", J_ROLE_OK, J_RESET);
    }

    // Extract intent
    const char* intent = extract_intent(args);
    if (intent) {
        jash_writef(w, ctx, "  Intent detected: %s%s%s\n", J_ROLE_OK, intent, J_RESET);
    }

    return 0;
}

static int cmd_compress(jash_writer_t w, void* ctx, const char* args) {
    if (!args || !*args) {
        jash_writef(w, ctx, "usage: prompt compress \"prompt\"\n");
        return 1;
    }

    char compressed[PROMPT_MAX];
    compress_prompt(args, compressed, sizeof(compressed));

    jash_writef(w, ctx, "%s%s%s\n", J_ROLE_OK, compressed, J_RESET);
    return 0;
}

static int cmd_stats(jash_writer_t w, void* ctx) {
    jash_writef(w, ctx, "%sPrompt Forge statistics:%s\n", J_ROLE_ACCENT, J_RESET);
    jash_writef(w, ctx, "  Patterns loaded: %d\n", (int)(sizeof(PATTERNS)/sizeof(PATTERNS[0]) - 1));
    jash_writef(w, ctx, "  Languages: Portuguese, English\n");
    jash_writef(w, ctx, "  Mode: Heuristic (no LLM required)\n");
    return 0;
}

// ── Dispatch ─────────────────────────────────────────────────────────────────

int jash_b_prompt(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    int selected = semantic_dispatch(s, w, ctx, "prompt", "jash.prompt.v1", args, prompt_semantic);
    if (selected >= 0) return selected;
    (void)s;
    if (!args || !*args) {
        jash_writef(w, ctx,
            "%sprompt%s — prompt optimization engine (no LLM required)\n\n"
            "Commands:\n"
            "  prompt optimize \"prompt\"     optimize prompt for token savings\n"
            "  prompt analyze \"prompt\"      analyze prompt for redundancy\n"
            "  prompt compress \"prompt\"     compress prompt preserving intent\n"
            "  prompt stats                 show optimization statistics\n",
            J_ROLE_BANNER, J_RESET);
        return 0;
    }

    char sub[64], rest[1024];
    rest[0] = '\0';
    int n = sscanf(args, "%63s %1023[^\n]", sub, rest);
    (void)n;

    if (strcmp(sub, "optimize") == 0) return cmd_optimize(w, ctx, rest);
    if (strcmp(sub, "analyze") == 0) return cmd_analyze(w, ctx, rest);
    if (strcmp(sub, "compress") == 0) return cmd_compress(w, ctx, rest);
    if (strcmp(sub, "stats") == 0) return cmd_stats(w, ctx);

    jash_writef(w, ctx, "%serror:%s unknown prompt subcommand '%s'\n",
                J_ROLE_ERR, J_RESET, sub);
    return 1;
}

SEMANTIC_ARGV_WRAPPER(prompt, "prompt", "jash.prompt.v1", prompt_semantic)
