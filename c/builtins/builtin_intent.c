// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — builtins/builtin_intent — LLM session intent anchor
//
// Usage:
//   intent "fixing the cmake linker error"   set current intent
//   intent                                    show current intent
//   intent clear                              clear intent
//
// The intent is:
//   1. Stored in JASH_INTENT env var (readable via printenv JASH_INTENT)
//   2. Persisted in ~/.jash/intent for next session
//   3. Shown in the prompt above the command line
//
// This gives LLMs a one-line anchor when working on long multi-step tasks.

#include <builtins/builtin.h>
#include <core/state.h>
#include <core/env.h>
#include <theme/io.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <sys/syscalls.h>
#include "memory_store.h"

static void intent_path(char* out, size_t sz) {
    const char* home = getenv("HOME");
    if (!home || !home[0]) home = jash_tmpdir();
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/.jash", home);
    mkdir(dir, 0755);
    snprintf(out, sz, "%s/.jash/intent", home);
}

static void intent_persist(const char* text) {
    char path[512];
    intent_path(path, sizeof(path));
    if (!text || !text[0]) {
        unlink(path);
        return;
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return;
    ssize_t nw __attribute__((unused));
    nw = write(fd, text, strlen(text));
    nw = write(fd, "\n", 1);
    close(fd);
}

// Called at shell start — restore persisted intent into JASH_INTENT.
void jash_intent_init(jash_state_t* s) {
    if (!s) return;
    // Don't override an already-set JASH_INTENT from parent env.
    const char* existing = jash_env_get(s, "JASH_INTENT");
    if (existing && existing[0]) return;

    /* Startup must neither create the store nor block on a nonregular file.
     * Native query reports malformed/unsafe persistence explicitly; startup
     * simply declines to seed a truncated or unchecked value. */
    memory_store_t store = {-1, -1};
    char* bytes = NULL; size_t length = 0; int exists = 0;
    int failed = memory_store_open(&store, NULL, 0) ||
                 memory_store_read(&store, "intent", 512, &bytes, &length, &exists);
    if (memory_store_close(&store)) failed = 1;
    if (!failed && exists) {
        if (length && bytes[length - 1] == '\n') bytes[--length] = 0;
        if (length && length <= 511 && !memchr(bytes, 0, length) &&
            !memchr(bytes, '\n', length) && !memchr(bytes, '\r', length))
            (void)jash_env_set(s, "JASH_INTENT", bytes);
    }
    free(bytes);
}

int jash_b_intent(jash_state_t* s, jash_writer_t w, void* ctx,
                  const char* args) {
    if (!s || !w) return 1;

    // Trim leading spaces.
    while (args && (*args == ' ' || *args == '\t')) args++;

    // No argument: show current (env var takes priority, then file).
    if (!args || !*args) {
        const char* cur = jash_env_get(s, "JASH_INTENT");
        if (cur && cur[0]) {
            jash_writef(w, ctx, "[intent] %s\r\n", cur);
            return 0;
        }
        // Fallback: read from file (works in -c mode and fresh shells).
        char path[512]; intent_path(path, sizeof(path));
        int fd = open(path, O_RDONLY);
        if (fd >= 0) {
            char buf[512]; ssize_t n = read(fd, buf, sizeof(buf) - 1); close(fd);
            if (n > 0) {
                buf[n] = '\0';
                while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r')) buf[--n] = '\0';
                if (n > 0) { jash_writef(w, ctx, "[intent] %s\r\n", buf); return 0; }
            }
        }
        jash_writes(w, ctx, "[intent] (none)\r\n");
        return 0;
    }

    // "clear" subcommand.
    if (args[0] == 'c' && args[1] == 'l' && args[2] == 'e' &&
        args[3] == 'a' && args[4] == 'r' &&
        (args[5] == '\0' || args[5] == ' ')) {
        jash_env_set(s, "JASH_INTENT", "");
        intent_persist(NULL);
        jash_writes(w, ctx, "[intent] cleared\r\n");
        return 0;
    }

    // Set intent — strip optional surrounding quotes.
    const char* text = args;
    size_t len = strlen(text);
    char buf[512];
    if (len >= 2 && ((text[0] == '"' && text[len-1] == '"') ||
                     (text[0] == '\'' && text[len-1] == '\''))) {
        len -= 2;
        if (len >= sizeof(buf)) len = sizeof(buf) - 1;
        memcpy(buf, text + 1, len);
        buf[len] = '\0';
        text = buf;
    }

    jash_env_set(s, "JASH_INTENT", text);
    intent_persist(text);
    jash_writef(w, ctx, "[intent] set: %s\r\n", text);
    return 0;
}

#include "intent_native.inc"
