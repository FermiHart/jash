// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Syntax Highlighting (superior to fish)

#include "syntax_highlight.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_TOKENS 256
#define MAX_TOKEN_LEN 4096

// Colors
#define COLOR_RESET   "\033[0m"
#define COLOR_RED     "\033[31m"
#define COLOR_GREEN   "\033[32m"
#define COLOR_YELLOW  "\033[33m"
#define COLOR_BLUE    "\033[34m"
#define COLOR_MAGENTA "\033[35m"
#define COLOR_CYAN    "\033[36m"
#define COLOR_WHITE   "\033[37m"
#define COLOR_BOLD    "\033[1m"

typedef enum {
    TOKEN_COMMAND,
    TOKEN_BUILTIN,
    TOKEN_FLAG,
    TOKEN_STRING,
    TOKEN_NUMBER,
    TOKEN_OPERATOR,
    TOKEN_COMMENT,
    TOKEN_PATH,
    TOKEN_VARIABLE
} token_type_t;

typedef struct {
    token_type_t type;
    int start;
    int end;
} token_t;

// Check if command exists
static int command_exists(const char* cmd) {
    if (!cmd || !*cmd) return 0;
    
    // Check if it's a builtin
    const char* builtins[] = {
        "cd", "pwd", "exit", "export", "unset", "alias", "unalias",
        "history", "jobs", "fg", "bg", "kill", "ls", "rm", "grep",
        "cp", "mv", "ps", "du", "echo", "help", "type", "which",
        NULL
    };
    
    for (int i = 0; builtins[i]; i++) {
        if (strcmp(cmd, builtins[i]) == 0) {
            return 2;  // Builtin
        }
    }
    
    // Check PATH
    const char* path_env = getenv("PATH");
    if (!path_env) return 0;
    
    char* path_copy = strdup(path_env);
    char* dir = strtok(path_copy, ":");
    
    while (dir) {
        char full_path[4096];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir, cmd);
        
        struct stat st;
        if (stat(full_path, &st) == 0 && (st.st_mode & S_IXUSR)) {
            free(path_copy);
            return 1;  // External command
        }
        dir = strtok(NULL, ":");
    }
    
    free(path_copy);
    return 0;
}

// Tokenize input
static int tokenize(const char* input, token_t* tokens, int max_tokens) {
    int ntokens = 0;
    int i = 0;
    int len = strlen(input);
    
    while (i < len && ntokens < max_tokens) {
        // Skip whitespace
        while (i < len && isspace(input[i])) i++;
        if (i >= len) break;
        
        int start = i;
        
        // Comment
        if (input[i] == '#') {
            while (i < len && input[i] != '\n') i++;
            tokens[ntokens].type = TOKEN_COMMENT;
            tokens[ntokens].start = start;
            tokens[ntokens].end = i;
            ntokens++;
            continue;
        }
        
        // String (single or double quotes)
        if (input[i] == '\'' || input[i] == '"') {
            char quote = input[i++];
            while (i < len && input[i] != quote) {
                if (input[i] == '\\' && i + 1 < len) i++;
                i++;
            }
            if (i < len) i++;
            tokens[ntokens].type = TOKEN_STRING;
            tokens[ntokens].start = start;
            tokens[ntokens].end = i;
            ntokens++;
            continue;
        }
        
        // Variable
        if (input[i] == '$') {
            i++;
            if (i < len && input[i] == '{') {
                while (i < len && input[i] != '}') i++;
                if (i < len) i++;
            } else {
                while (i < len && (isalnum(input[i]) || input[i] == '_')) i++;
            }
            tokens[ntokens].type = TOKEN_VARIABLE;
            tokens[ntokens].start = start;
            tokens[ntokens].end = i;
            ntokens++;
            continue;
        }
        
        // Operator
        if (strchr("|&;<>()", input[i])) {
            i++;
            if (i < len && input[i] == input[i-1]) i++;  // ||, &&, >>
            tokens[ntokens].type = TOKEN_OPERATOR;
            tokens[ntokens].start = start;
            tokens[ntokens].end = i;
            ntokens++;
            continue;
        }
        
        // Flag
        if (input[i] == '-') {
            i++;
            if (i < len && input[i] == '-') i++;  // --
            while (i < len && (isalnum(input[i]) || input[i] == '-' || input[i] == '=')) i++;
            tokens[ntokens].type = TOKEN_FLAG;
            tokens[ntokens].start = start;
            tokens[ntokens].end = i;
            ntokens++;
            continue;
        }
        
        // Number
        if (isdigit(input[i])) {
            while (i < len && isdigit(input[i])) i++;
            tokens[ntokens].type = TOKEN_NUMBER;
            tokens[ntokens].start = start;
            tokens[ntokens].end = i;
            ntokens++;
            continue;
        }
        
        // Word (command or argument)
        while (i < len && !isspace(input[i]) && !strchr("|&;<>()", input[i])) {
            if (input[i] == '\\' && i + 1 < len) i++;
            i++;
        }
        
        // Determine if it's a command, path, or regular word
        char word[MAX_TOKEN_LEN];
        int word_len = i - start;
        if (word_len >= MAX_TOKEN_LEN) word_len = MAX_TOKEN_LEN - 1;
        strncpy(word, input + start, word_len);
        word[word_len] = '\0';
        
        if (ntokens == 0 || (ntokens > 0 && tokens[ntokens-1].type == TOKEN_OPERATOR)) {
            int cmd_type = command_exists(word);
            tokens[ntokens].type = (cmd_type == 2) ? TOKEN_BUILTIN : TOKEN_COMMAND;
        } else if (strchr(word, '/')) {
            tokens[ntokens].type = TOKEN_PATH;
        } else {
            tokens[ntokens].type = TOKEN_STRING;
        }
        
        tokens[ntokens].start = start;
        tokens[ntokens].end = i;
        ntokens++;
    }
    
    return ntokens;
}

// Get color for token type
static const char* get_color(token_type_t type, int cmd_exists) {
    switch (type) {
        case TOKEN_COMMAND:
            return cmd_exists ? COLOR_GREEN : COLOR_RED;
        case TOKEN_BUILTIN:
            return COLOR_BOLD COLOR_CYAN;
        case TOKEN_FLAG:
            return COLOR_CYAN;
        case TOKEN_STRING:
            return COLOR_YELLOW;
        case TOKEN_NUMBER:
            return COLOR_MAGENTA;
        case TOKEN_OPERATOR:
            return COLOR_WHITE;
        case TOKEN_COMMENT:
            return "\033[90m";  // Gray
        case TOKEN_PATH:
            return COLOR_BLUE;
        case TOKEN_VARIABLE:
            return COLOR_MAGENTA;
        default:
            return COLOR_RESET;
    }
}

// Highlight a line
char* syntax_highlight(const char* input) {
    if (!input || !*input) return strdup("");
    
    token_t tokens[MAX_TOKENS];
    int ntokens = tokenize(input, tokens, MAX_TOKENS);
    
    // Calculate output size
    int input_len = strlen(input);
    int output_size = input_len * 4;  // Worst case: every char gets color codes
    char* output = malloc(output_size);
    if (!output) return strdup(input);
    
    int out_pos = 0;
    int in_pos = 0;
    
    for (int i = 0; i < ntokens; i++) {
        // Add whitespace before token
        while (in_pos < tokens[i].start) {
            output[out_pos++] = input[in_pos++];
        }
        
        // Add color code
        int cmd_exists = (tokens[i].type == TOKEN_COMMAND) ? command_exists(input + tokens[i].start) : 1;
        const char* color = get_color(tokens[i].type, cmd_exists);
        int color_len = strlen(color);
        memcpy(output + out_pos, color, color_len);
        out_pos += color_len;
        
        // Add token
        int token_len = tokens[i].end - tokens[i].start;
        memcpy(output + out_pos, input + tokens[i].start, token_len);
        out_pos += token_len;
        in_pos = tokens[i].end;
        
        // Add reset
        const char* reset = COLOR_RESET;
        int reset_len = strlen(reset);
        memcpy(output + out_pos, reset, reset_len);
        out_pos += reset_len;
    }
    
    // Add remaining input
    while (in_pos < input_len) {
        output[out_pos++] = input[in_pos++];
    }
    
    output[out_pos] = '\0';
    return output;
}

// Free highlighted string
void syntax_highlight_free(char* highlighted) {
    free(highlighted);
}
