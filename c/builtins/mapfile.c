// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// Builtin: mapfile / readarray — read lines into array variable

#include "builtin.h"
#include <core/state.h>
#include <core/env.h>
#include <core/readbuf.h>
#include <theme/io.h>
#include <sys/syscalls.h>
#include <string.h>
#include <stdio.h>

int jash_b_mapfile(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    (void)ctx;
    if (!args || !*args) {
        const char* msg = "mapfile: usage: mapfile [-d delim] [-n count] [-s count] [-t] array\n";
        w(0, msg, strlen(msg));
        return 1;
    }

    /* Parse args: mapfile [-d X] [-n N] [-s N] [-t] ARRAYNAME */
    char delim = '\n';
    int skip = 0;
    int count = -1;
    int trim = 0;  /* -t: strip trailing delimiter from each element */
    char array_name_buf[128];
    const char* array_name = NULL;

    const char* p = args;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;

        if (p[0] == '-' && p[1] == 'd' && p[2] == ' ') {
            p += 3;
            while (*p == ' ' || *p == '\t') p++;
            delim = *p++;
        } else if (p[0] == '-' && p[1] == 'n' && p[2] == ' ') {
            p += 3;
            while (*p == ' ' || *p == '\t') p++;
            count = 0;
            while (*p >= '0' && *p <= '9') count = count * 10 + (*p++ - '0');
        } else if (p[0] == '-' && p[1] == 's' && p[2] == ' ') {
            p += 3;
            while (*p == ' ' || *p == '\t') p++;
            skip = 0;
            while (*p >= '0' && *p <= '9') skip = skip * 10 + (*p++ - '0');
        } else if (p[0] == '-' && p[1] == 't' && (p[2] == ' ' || p[2] == '\t' || p[2] == '\0')) {
            trim = 1;
            if (p[2] == ' ' || p[2] == '\t') p += 3;
            else p += 2;
        } else {
            /* Array name */
            const char* start = p;
            while (*p && *p != ' ' && *p != '\t') p++;
            unsigned len = p - start;
            if (len > 0 && len < 128) {
                for (unsigned i = 0; i < len; i++) array_name_buf[i] = start[i];
                array_name_buf[len] = '\0';
                array_name = array_name_buf;
            }
        }
    }

    if (!array_name) {
        const char* msg = "mapfile: array name required\n";
        w(0, msg, strlen(msg));
        return 1;
    }

    /* Read from stdin using buffered I/O */
    jash_readbuf_t rb;
    jash_readbuf_init(&rb, 0);
    char line[4096];
    unsigned line_pos = 0;
    int line_idx = 0;
    int lines_read = 0;

    for (;;) {
        char c;
        int rc = jash_readbuf_getc(&rb, &c);
        if (rc <= 0) break;

        if (c == delim) {
            /* Without -t: include the delimiter in the stored element (bash default).
             * With -t: strip it (current line buffer already excludes delimiter). */
            if (!trim && line_pos < sizeof(line) - 1)
                line[line_pos++] = c;
            line[line_pos] = '\0';
            if (skip > 0) {
                skip--;
            } else {
                char key[256];
                snprintf(key, sizeof(key), "%s[%d]", array_name, line_idx);
                jash_env_set(s, key, line);
                line_idx++;
                lines_read++;
                if (count >= 0 && lines_read >= count) {
                    line_pos = 0;  /* clear stale buffer before trailing-line check */
                    goto mapfile_done;
                }
            }
            line_pos = 0;
        } else {
            if (line_pos < sizeof(line) - 1) {
                line[line_pos++] = c;
            }
        }
    }
mapfile_done:

    /* Handle last line without delimiter */
    if (line_pos > 0 && skip <= 0) {
        line[line_pos] = '\0';
        char key[256];
        snprintf(key, sizeof(key), "%s[%d]", array_name, line_idx);
        jash_env_set(s, key, line);
    }

    return 0;
}
