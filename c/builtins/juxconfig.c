// SPDX-License-Identifier: LicenseRef-SPL-1.0
// Builtin: juxconfig — compact BearJash UX configuration.

#include "builtin.h"
#include <core/env.h>
#include <string.h>

static const char* skip_ws(const char* p) { while (*p == ' ' || *p == '\t') p++; return p; }

int jash_b_juxconfig(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    const char* p = skip_ws(args ? args : "");
    if (!*p || strcmp(p, "show") == 0) {
        const char* autosuggest = jash_env_get(s, "JASH_AUTOSUGGEST");
        const char* color = jash_env_get(s, "JASH_AUTOSUGGEST_COLOR");
        jash_writef(w, ctx, "autosuggest=%s\n", autosuggest ? autosuggest : "1");
        jash_writef(w, ctx, "autosuggest_color=%s\n", color ? color : "\\033[90m");
        jash_writes(w, ctx, "usage: juxconfig autosuggest on|off | juxconfig theme dark|light\n");
        return 0;
    }
    if (strncmp(p, "autosuggest ", 12) == 0) {
        const char* v = skip_ws(p + 12);
        if (strcmp(v, "off") == 0 || strcmp(v, "0") == 0) jash_env_set(s, "JASH_AUTOSUGGEST", "0");
        else jash_env_set(s, "JASH_AUTOSUGGEST", "1");
        return 0;
    }
    if (strncmp(p, "theme ", 6) == 0) {
        const char* v = skip_ws(p + 6);
        if (strcmp(v, "light") == 0) jash_env_set(s, "JASH_AUTOSUGGEST_COLOR", "\033[2m");
        else jash_env_set(s, "JASH_AUTOSUGGEST_COLOR", "\033[90m");
        return 0;
    }
    jash_writes(w, ctx, "juxconfig: unknown command\n");
    return 2;
}
