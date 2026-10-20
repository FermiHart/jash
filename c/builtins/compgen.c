// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell
// Builtin: compgen — generate completion matches (bash compat)

#include "builtin.h"
#include <string.h>
#include <stdio.h>
#include <dirent.h>
#include <core/env.h>

static int starts_with(const char* s, const char* prefix) {
    while (*prefix) { if (*s != *prefix) return 0; s++; prefix++; }
    return 1;
}

static void print_match(const char* name, jash_writer_t w, void* ctx) {
    jash_writef(w, ctx, "%s\n", name);
}

int jash_b_compgen(jash_state_t* s, jash_writer_t w, void* ctx,
                   const char* args) {
    (void)s;
    if (!args) { args = ""; }

    const char* p = args;
    while (*p == ' ') p++;

    /* Default: complete commands */
    int do_commands = 1;
    int do_files = 0;
    int do_variables = 0;
    const char* word = "";

    /* Parse -A action */
    if (*p == '-' && *(p+1) == 'A') {
        p += 2;
        while (*p == ' ') p++;
        if (starts_with(p, "command")) { do_commands = 1; do_files = 0; p += 7; }
        else if (starts_with(p, "file")) { do_files = 1; do_commands = 0; p += 4; }
        else if (starts_with(p, "variable")) { do_variables = 1; do_commands = 0; p += 8; }
        else if (starts_with(p, "builtin")) { do_commands = 1; do_files = 0; p += 7; }
        while (*p == ' ') p++;
    }

    /* Parse -W wordlist */
    if (*p == '-' && *(p+1) == 'W') {
        p += 2;
        while (*p == ' ') p++;
        word = p;
        while (*p && *p != ' ') p++;
        /* Print wordlist matches */
        const char* wp = word;
        while (*wp) {
            while (*wp == ' ') wp++;
            if (!*wp) break;
            const char* ws = wp;
            while (*wp && *wp != ' ') wp++;
            uint32_t wlen = (uint32_t)(wp - ws);
            char wname[256];
            if (wlen < sizeof(wname)) {
                for (uint32_t i = 0; i < wlen; i++) wname[i] = ws[i];
                wname[wlen] = '\0';
                print_match(wname, w, ctx);
            }
        }
        return 0;
    }

    /* Remaining args is the prefix */
    while (*p == ' ') p++;
    const char* prefix = p;

    if (do_commands) {
        /* Builtins */
        uint32_t total = (uint32_t)jash_builtin_count();
        for (uint32_t i = 0; i < total; i++) {
            const jash_builtin_t* b = jash_builtin_at(i);
            if (b && b->name && starts_with(b->name, prefix))
                print_match(b->name, w, ctx);
        }
        /* PATH executables */
        const char* path = jash_env_get(s, "PATH");
        if (!path) path = "/bin:/usr/bin";
        const char* pp = path;
        while (*pp) {
            const char* start = pp;
            while (*pp && *pp != ':') pp++;
            uint32_t dirlen = (uint32_t)(pp - start);
            if (dirlen > 0) {
                char dirpath[256];
                uint32_t k = 0;
                for (uint32_t i = 0; i < dirlen && k + 1 < sizeof(dirpath); i++)
                    dirpath[k++] = start[i];
                dirpath[k] = '\0';
                DIR* dir = opendir(dirpath);
                if (dir) {
                    struct dirent* entry;
                    while ((entry = readdir(dir)) != NULL) {
                        if (starts_with(entry->d_name, prefix))
                            print_match(entry->d_name, w, ctx);
                    }
                    closedir(dir);
                }
            }
            if (*pp == ':') pp++;
        }
    }

    if (do_files) {
        const char* dir = ".";
        const char* fprefix = prefix;
        /* Split at last / */
        const char* slash = NULL;
        for (int i = (int)strlen(prefix) - 1; i >= 0; i--) {
            if (prefix[i] == '/') { slash = &prefix[i]; break; }
        }
        char dirbuf[256] = ".";
        if (slash) {
            uint32_t dl = (uint32_t)(slash - prefix);
            if (dl >= sizeof(dirbuf)) dl = sizeof(dirbuf) - 1;
            for (uint32_t i = 0; i < dl; i++) dirbuf[i] = prefix[i];
            dirbuf[dl] = '\0';
            dir = dirbuf;
            fprefix = slash + 1;
        }
        DIR* d = opendir(dir);
        if (d) {
            struct dirent* entry;
            while ((entry = readdir(d)) != NULL) {
                if (starts_with(entry->d_name, fprefix)) {
                    char full[512];
                    if (slash) {
                        snprintf(full, sizeof(full), "%s/%s", dirbuf, entry->d_name);
                    } else {
                        snprintf(full, sizeof(full), "%s", entry->d_name);
                    }
                    print_match(full, w, ctx);
                }
            }
            closedir(d);
        }
    }

    if (do_variables) {
        uint32_t n = jash_env_count(s);
        for (uint32_t i = 0; i < n; i++) {
            const char* k = NULL;
            jash_env_at(s, i, &k, NULL);
            if (k && starts_with(k, prefix)) {
                jash_writef(w, ctx, "$%s\n", k);
            }
        }
    }

    return 0;
}
