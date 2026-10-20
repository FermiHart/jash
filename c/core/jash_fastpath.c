// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash_fastpath.c — Zero-LLM Smalltalk Classifier implementation
// Bilingual PT/EN smalltalk detection with vocative stripping.
// F E R M I ∞ H A R T

#include "jash_fastpath.h"

#include <string.h>
#include <ctype.h>

#define FASTPATH_MAX_INPUT_TOKENS 6

// ── User Name for Vocative Stripping ─────────────────────────────────────────

static char g_user_name[64];
static int  g_user_name_len;

void jash_fastpath_set_user_name(const char* name) {
    if (!name) { g_user_name[0] = '\0'; g_user_name_len = 0; return; }
    size_t n = strlen(name);
    if (n >= sizeof(g_user_name)) n = sizeof(g_user_name) - 1;
    memcpy(g_user_name, name, n);
    g_user_name[n] = '\0';
    g_user_name_len = (int)n;
}

// ── Tokenizer Helpers ────────────────────────────────────────────────────────

// Normalize: lowercase, strip punctuation at boundaries
static void normalize_word(const char* src, size_t src_len, char* dst, size_t dst_cap) {
    size_t j = 0;
    for (size_t i = 0; i < src_len && j + 1 < dst_cap; i++) {
        char c = src[i];
        if (isalpha((unsigned char)c)) {
            dst[j++] = (char)tolower((unsigned char)c);
        }
    }
    dst[j] = '\0';
}

// Check if word matches a NULL-terminated list (case-insensitive)
static int word_in_list(const char* word, const char* list[]) {
    for (int i = 0; list[i]; i++) {
        if (strcmp(word, list[i]) == 0) return 1;
    }
    return 0;
}

// ── Vocabularies ─────────────────────────────────────────────────────────────

static const char* greetings[] = {
    "oi", "ola", "olá", "hey", "hi", "hello", "alo", "alô",
    "bom dia", "boa tarde", "boa noite",
    "good morning", "good afternoon", "good evening",
    "eai", "e aí", "fala", "falae", "fallae",
    "salve", "opa", "eae",
    "sup", "yo", "howdy", "whatsup", "wassup",
    "iae", "iae,",
    NULL
};

static const char* thanks[] = {
    "obrigado", "obrigada", "valeu", "brigado",
    "thanks", "thank you", "thankyou", "thx", "ty",
    "gracias", "merci", "danke",
    "valeu", "vlw", "tmj",
    "agradecido", "agradecida",
    "appreciate", "grateful",
    NULL
};

static const char* farewells[] = {
    "tchau", "bye", "goodbye", "good bye", "see you",
    "ate logo", "até logo", "ate mais", "até mais",
    "falou", "flw", "fui",
    "cya", "later", "seeya",
    "ate", "até",
    "nos vemos", "see ya",
    NULL
};

static const char* reactions[] = {
    "legal", "show", "nice", "awesome", "top",
    "massa", "da hora", "sinistro", "brabo",
    "cool", "great", "perfect", "excellent",
    "caramba", "nossa", "uau", "wow",
    "demais", "incrivel", "incrível",
    "amazing", "fantastic", "brilliant",
    "muito bom", "mt bom", "very good", "well done",
    "isso ai", "isso aí", "that's it",
    "correto", "correto", "right", "exactly",
    NULL
};

static const char* agrees[] = {
    "sim", "s", "claro", "com certeza", "logico", "lógico",
    "yes", "y", "yeah", "yep", "sure", "absolutely",
    "ok", "okay", "ta bom", "tá", "pode ser",
    "concordo", "agreed",
    "verdade", "true",
    "isso", "exato",
    "beleza", "blz", "fechou",
    "vamos", "bora",
    "go ahead", "do it",
    NULL
};

static const char* disagrees[] = {
    "nao", "não", "num", "nem",
    "no", "nope", "never", "nah",
    "discordo", "nada", "jamais",
    "negative", "nix",
    "claro que nao", "claro que não",
    "de jeito nenhum",
    "no way", "no way",
    NULL
};

static const char* confusions[] = {
    "como", "como?", "como assim",
    "what", "what?", "huh", "hmm",
    "nao entendi", "não entendi", "nao entendi",
    "nao entendo", "não entendo",
    "i don't understand", "i don't get it",
    "explain", "repeat",
    "como disse", "que",
    "sorry", "desculpa", "desculpe",
    "?", "??", "???",
    "nao sei", "não sei", "i don't know",
    NULL
};

// ── Stopwords (short tokens to ignore) ────────────────────────────────────────

static int is_stopword(const char* w) {
    static const char* stop[] = {
        "a", "e", "o", "de", "da", "do", "em", "para", "por", "com",
        "the", "and", "or", "of", "to", "in", "for", "with",
        "um", "uma", "uns", "umas",
        "a", "as", "os", "as",
        "meu", "minha", "seu", "sua",
        "my", "your", "his", "her", "its", "our", "their",
        "que", "what", "who", "where", "when", "why", "how",
        "isso", "isto", "aquilo", "this", "that",
        NULL
    };
    for (int i = 0; stop[i]; i++) {
        if (strcmp(w, stop[i]) == 0) return 1;
    }
    if (strlen(w) <= 1) return 1;  // single chars are noise
    return 0;
}

// ── Vocative Stripping ───────────────────────────────────────────────────────

// If input starts with the user's name (e.g., "Fermi, o que é isso?"),
// skip past it. Returns pointer past the vocative, or original input.
static const char* strip_vocative(const char* input, int* had_vocative) {
    *had_vocative = 0;
    if (g_user_name_len == 0 || !input) return input;

    // Check if input starts with user name (case-insensitive)
    const char* p = input;
    while (*p && isspace((unsigned char)*p)) p++;

    if (strncasecmp(p, g_user_name, (size_t)g_user_name_len) == 0) {
        p += g_user_name_len;
        // Skip punctuation/space after name
        while (*p && (isspace((unsigned char)*p) || *p == ',' || *p == ':' || *p == '!'))
            p++;
        *had_vocative = 1;
        return p;
    }

    return input;
}

// ── Classification ───────────────────────────────────────────────────────────

jash_fastpath_result_t jash_fastpath_classify(const char* input) {
    jash_fastpath_result_t result;
    memset(&result, 0, sizeof(result));

    if (!input || !input[0]) return result;

    // Strip vocative
    int had_voc = 0;
    const char* body = strip_vocative(input, &had_voc);
    result.had_vocative = had_voc;

    // Tokenize into normalized words
    char words[32][32];
    int word_count = 0;
    int input_token_count = 0;

    const char* p = body;
    while (*p && word_count < 32) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;

        /* This is only a small-talk shortcut. Longer sentences must reach the
         * AI router even when an ordinary word happens to match a vocabulary
         * item (for example Portuguese "no makefile" contains English "no"). */
        if (++input_token_count > FASTPATH_MAX_INPUT_TOKENS) return result;

        const char* start = p;
        while (*p && !isspace((unsigned char)*p)) p++;
        size_t len = (size_t)(p - start);

        char norm[64];
        normalize_word(start, len, norm, sizeof(norm));

        if (norm[0] && !is_stopword(norm)) {
            // Deduplicate
            int dup = 0;
            for (int i = 0; i < word_count; i++) {
                if (strcmp(words[i], norm) == 0) { dup = 1; break; }
            }
            if (!dup) {
                size_t n = strlen(norm);
                if (n >= sizeof(words[0])) n = sizeof(words[0]) - 1;
                memcpy(words[word_count], norm, n);
                words[word_count][n] = '\0';
                word_count++;
            }
        }
    }

    if (word_count == 0) return result;

    // Classify by matching words against vocabularies
    int match_counts[8] = {0};
    float conf[8] = {0};

    for (int w = 0; w < word_count; w++) {
        const char* word = words[w];

        if (word_in_list(word, greetings)) {
            match_counts[FASTPATH_GREETING]++;
            conf[FASTPATH_GREETING] = 0.95f;
        }
        if (word_in_list(word, thanks)) {
            match_counts[FASTPATH_THANKS]++;
            conf[FASTPATH_THANKS] = 0.95f;
        }
        if (word_in_list(word, farewells)) {
            match_counts[FASTPATH_FAREWELL]++;
            conf[FASTPATH_FAREWELL] = 0.90f;
        }
        if (word_in_list(word, reactions)) {
            match_counts[FASTPATH_REACTION]++;
            conf[FASTPATH_REACTION] = 0.85f;
        }
        if (word_in_list(word, agrees)) {
            match_counts[FASTPATH_AGREE]++;
            conf[FASTPATH_AGREE] = 0.85f;
        }
        if (input_token_count == 1 && word_in_list(word, disagrees)) {
            match_counts[FASTPATH_DISAGREE]++;
            conf[FASTPATH_DISAGREE] = 0.85f;
        }
        if (word_in_list(word, confusions)) {
            match_counts[FASTPATH_CONFUSION]++;
            conf[FASTPATH_CONFUSION] = 0.80f;
        }
    }

    // Find best match
    int best_kind = FASTPATH_NONE;
    float best_conf = 0;
    int best_matches = 0;

    for (int k = 1; k <= FASTPATH_CONFUSION; k++) {
        if (match_counts[k] > best_matches ||
            (match_counts[k] == best_matches && conf[k] > best_conf)) {
            best_kind = k;
            best_conf = conf[k];
            best_matches = match_counts[k];
        }
    }

    if (best_matches == 0) return result;

    result.kind = (jash_fastpath_kind_t)best_kind;
    result.confidence = best_conf;

    // Static responses
    switch (result.kind) {
        case FASTPATH_GREETING:
            result.response = "Olá! Como posso ajudar?";
            break;
        case FASTPATH_THANKS:
            result.response = "Por nada! Sempre às ordens.";
            break;
        case FASTPATH_FAREWELL:
            result.response = "Até mais! Qualquer coisa, estou aqui.";
            break;
        case FASTPATH_REACTION:
            result.response = "Fico feliz que gostou!";
            break;
        case FASTPATH_AGREE:
            result.response = "Beleza, vamos nessa!";
            break;
        case FASTPATH_DISAGREE:
            result.response = "Entendo. Pode explicar melhor?";
            break;
        case FASTPATH_CONFUSION:
            result.response = "Deixa eu explicar melhor...";
            break;
        default:
            break;
    }

    return result;
}
