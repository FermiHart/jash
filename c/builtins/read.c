// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Builtin: read — bash-compatible line reader core.

#include "builtin.h"
#include <core/env.h>
#include <core/readbuf.h>
#include <sys/syscalls.h>
#include <string.h>
#include <stdio.h>
#include <poll.h>
#include <errno.h>
#ifndef JASH_AGENT_ONLY
#include <readline/loop.h>
#endif

static const char* skip_ws_local(const char* p) {
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

/* IFS-aware field splitting for the line `read` consumed. Was hardcoded to
 * ' '/'\t' only — `IFS=: read x y z` with input "a:b:c" left x="a:b:c",
 * y="", z="" instead of splitting on ':'. Mirrors enough of POSIX field
 * splitting for read's common case: IFS whitespace chars (space/tab/
 * newline, when present in IFS) collapse as a run and get trimmed at the
 * boundary; any other IFS char is an exact single-character delimiter
 * (so "a::b" with IFS=: yields an empty field between the two colons,
 * same as bash). */
static int read_is_ifs_char(const char* ifs, char c) {
    for (const char* q = ifs; *q; q++) if (*q == c) return 1;
    return 0;
}
static int read_is_ifs_ws(const char* ifs, char c) {
    return (c == ' ' || c == '\t' || c == '\n') && read_is_ifs_char(ifs, c);
}
static const char* read_skip_ifs_ws(const char* ifs, const char* p) {
    while (*p && read_is_ifs_ws(ifs, *p)) p++;
    return p;
}

static int parse_uint_arg(const char** p) {
    int n = 0;
    *p = skip_ws_local(*p);
    while (**p >= '0' && **p <= '9') {
        n = n * 10 + (**p - '0');
        (*p)++;
    }
    return n;
}

static void next_token(const char** p, char* out, unsigned cap) {
    unsigned o = 0;
    *p = skip_ws_local(*p);
    while (**p && **p != ' ' && **p != '\t' && o + 1 < cap) {
        out[o++] = **p;
        (*p)++;
    }
    out[o] = '\0';
}

static void set_array_words(jash_state_t* s, const char* name, const char* line) {
    int idx = 0;
    const char* p = line;
    while (*p) {
        p = skip_ws_local(p);
        if (!*p) break;
        const char* start = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        unsigned len = (unsigned)(p - start);
        char key[128];
        char val[512];
        if (len >= sizeof(val)) len = sizeof(val) - 1;
        for (unsigned i = 0; i < len; i++) val[i] = start[i];
        val[len] = '\0';
        snprintf(key, sizeof(key), "%s[%d]", name, idx++);
        jash_env_set(s, key, val);
    }
}

/* A private buffer is safe only when unread bytes can be returned to the open
 * file description. Pipes/terminals keep one-byte reads; no FILE* buffer or
 * cross-call cache may hide data from the next builtin or external command. */
static int read_regular_input(int fd) {
    struct stat st;
    return fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && lseek(fd, 0, SEEK_CUR) >= 0;
}

static long read_input_byte(jash_readbuf_t* input, int buffered, char* out) {
    if (!buffered) return sys_read(input->fd, out, 1);
    int rc;
    do { rc = jash_readbuf_getc(input, out); } while (rc < 0 && errno == EINTR);
    return rc;
}

static int read_return_unread(jash_readbuf_t* input) {
    if (input->len <= input->pos) return 0;
    off_t back = (off_t)input->pos - (off_t)input->len;
    off_t rc;
    do { rc = lseek(input->fd, back, SEEK_CUR); } while (rc < 0 && errno == EINTR);
    if (rc < 0) {
        jash_writes(jash_stderr_writer, 0, "read: unable to restore input position\n");
        return -1;
    }
    return 0;
}

static int read_line_reserve(char** line, char** heap, size_t* cap, size_t used) {
    if (used + 1 < *cap) return 0;
    if (*cap > SIZE_MAX / 2) return -1;
    size_t new_cap = *cap * 2;
    char* next = realloc(*heap, new_cap);
    if (!next) return -1;
    if (!*heap) memcpy(next, *line, used);
    *heap = next;
    *line = next;
    *cap = new_cap;
    return 0;
}

int jash_b_read_from_fd(jash_state_t *s, jash_writer_t w, void *ctx,
                       const char *args, int input_fd) {
    const char* p = args ? args : "";
    const char* prompt = NULL;
    char prompt_buf[256];
    char array_name[64];
    int raw = 0;
    int silent = 0;
    int max_chars = -1;
    int use_array = 0;
    int use_readline = 0;
    int timeout_ms = -1;
    const char* initial_text = NULL;
    char delim_char = '\n';

    while (1) {
        p = skip_ws_local(p);
        if (*p != '-') break;
        p++;
        while (*p && *p != ' ' && *p != '\t') {
            char opt = *p++;
            if (opt == 'r') raw = 1;
            else if (opt == 's') silent = 1;
            else if (opt == 'n') max_chars = parse_uint_arg(&p);
            else if (opt == 'd') {
                p = skip_ws_local(p);
                if (*p) delim_char = *p++;
                /* -d takes the NEXT argument if separated by space */
                if (!delim_char) {
                    p = skip_ws_local(p);
                    delim_char = *p ? *p++ : '\0';
                }
                break; /* -d consumes to end of this flag group */
            }
            else if (opt == 'e') use_readline = 1;
            else if (opt == 'i') {
                p = skip_ws_local(p);
                /* Grab initial text (up to next space or end) */
                initial_text = p;
                while (*p && *p != ' ' && *p != '\t') p++;
            }
            else if (opt == 'p') {
                p = skip_ws_local(p);
                unsigned pi = 0;
                char quote = 0;
                if (*p == '"' || *p == '\'') quote = *p++;
                while (*p && pi + 1 < sizeof(prompt_buf)) {
                    if (quote) {
                        if (*p == quote) { p++; break; }
                    } else if (*p == ' ' || *p == '\t') break;
                    prompt_buf[pi++] = *p++;
                }
                prompt_buf[pi] = '\0';
                prompt = prompt_buf;
            } else if (opt == 'a') {
                p = skip_ws_local(p);
                next_token(&p, array_name, sizeof(array_name));
                use_array = array_name[0] != '\0';
            } else if (opt == 't') {
                /* -t TIMEOUT  (integer or decimal seconds; can follow a space) */
                p = skip_ws_local(p);
                int tv = 0, frac = 0, frac_div = 1;
                while (*p >= '0' && *p <= '9') { tv = tv * 10 + (*p - '0'); p++; }
                if (*p == '.') {
                    p++;
                    while (*p >= '0' && *p <= '9') {
                        frac = frac * 10 + (*p - '0');
                        frac_div *= 10;
                        p++;
                    }
                }
                timeout_ms = tv * 1000 + (frac * 1000 / frac_div);
            }
        }
    }

    (void)use_readline;
    (void)initial_text;
    if (prompt) jash_writes(w, ctx, prompt);
    (void)silent; /* Non-interactive reads do not echo input anyway. */

    char line_inline[1024];
    char* line = line_inline;
    char* line_heap = NULL;
    size_t line_cap = sizeof(line_inline);
    size_t li = 0;
    int read_status = 1; /* EOF before the delimiter/count is a partial read. */

#ifndef JASH_AGENT_ONLY
    if (use_readline && s->interactive) {
        /* Use the readline engine for input */
        char rl_buf[1024];
        int rl_rc = jash_read_line(s, (jash_reader_t)0, (void*)0,
                                   w, ctx, rl_buf, sizeof(rl_buf));
        if (rl_rc == 0) {
            unsigned rlen = 0;
            while (rl_buf[rlen] && rlen + 1 < line_cap) {
                line[rlen] = rl_buf[rlen];
                rlen++;
            }
            line[rlen] = '\0';
            li = rlen;
            read_status = 0;
        } else {
            li = 0;
            line[0] = '\0';
        }
    } else
#endif
    {
        if (max_chars == 0) { line[0] = '\0'; read_status = 0; goto read_done; }
        /* -t: poll before blocking to support timeout */
        if (timeout_ms >= 0) {
            struct pollfd pfd = { .fd = input_fd, .events = POLLIN, .revents = 0 };
            if (poll(&pfd, 1, timeout_ms) <= 0) {
                line[0] = '\0'; li = 0;
                goto read_done;
            }
        }
        int buffered = raw && delim_char == '\n' && max_chars < 0 &&
            timeout_ms < 0 && !use_array && !use_readline && !initial_text &&
            read_regular_input(input_fd);
        jash_readbuf_t input;
        jash_readbuf_init(&input, input_fd);
        int memory_error = 0;
        for (;;) {
            if (read_line_reserve(&line, &line_heap, &line_cap, li) < 0) {
                memory_error = 1;
                break;
            }
            char c;
            long n = read_input_byte(&input, buffered, &c);
            if (n <= 0) break;
            if (c == delim_char) { read_status = 0; break; }
            if (c == '\0') continue; /* Shell variables cannot contain NUL. */
            if (!raw && c == '\\' && delim_char == '\n') {
                long m = read_input_byte(&input, buffered, &c);
                if (m <= 0) break;
                if (c == '\n') continue;
            }
            line[li++] = c;
            if (max_chars >= 0 && li >= (size_t)max_chars) { read_status = 0; break; }
        }
        line[li] = '\0';
        int offset_error = buffered && read_return_unread(&input) < 0;
        if (memory_error || offset_error) {
            if (memory_error)
                jash_writes(jash_stderr_writer, 0, "read: out of memory for input line\n");
            free(line_heap);
            return 1;
        }
    }
read_done:;

    /* Apply -i initial text: prepend to the read line */
    if (initial_text && initial_text[0] && li == 0) {
        unsigned ilen = 0;
        while (initial_text[ilen] && ilen + 1 < line_cap) {
            line[ilen] = initial_text[ilen];
            ilen++;
        }
        line[ilen] = '\0';
        li = ilen;
    }

    if (use_array) {
        set_array_words(s, array_name, line);
        free(line_heap);
        return read_status;
    }

    char vars[16][64];
    int var_count = 0;
    while (*p && var_count < 16) {
        next_token(&p, vars[var_count], sizeof(vars[0]));
        if (vars[var_count][0]) var_count++;
    }
    if (var_count == 0) {
        if (jash_env_set(s, "REPLY", line) != 0) read_status = 1;
        free(line_heap);
        return read_status;
    }

    const char* ifs = jash_env_get(s, "IFS");
    if (!ifs) ifs = " \t\n";

    char* lp = (char*)read_skip_ifs_ws(ifs, line);
    for (int vi = 0; vi < var_count; vi++) {
        char* start = lp;
        char delim_seen = '\0';
        if (vi == var_count - 1) {
            while (*lp) lp++;
        } else {
            while (*lp && !read_is_ifs_char(ifs, *lp)) lp++;
            if (*lp) { delim_seen = *lp; lp++; }
        }
        size_t len = (size_t)(lp - start);
        /* Don't swallow the delimiter itself into the field's tail. */
        if (delim_seen && len > 0) len--;
        if (raw && vi == var_count - 1)
            while (len && read_is_ifs_ws(ifs, start[len - 1])) len--;
        /* Assignment owns its copy; temporarily terminate a field in-place
         * rather than truncating it into the old 512-byte intermediate. */
        char saved = start[len];
        start[len] = '\0';
        if (jash_env_set(s, vars[vi], start) != 0) read_status = 1;
        start[len] = saved;
        /* A whitespace-class delimiter collapses the whole following run
         * (bash semantics); a non-whitespace IFS char (e.g. ':') is exact —
         * consecutive ones produce empty fields, so don't skip more of them. */
        if (delim_seen && read_is_ifs_ws(ifs, delim_seen)) lp = (char*)read_skip_ifs_ws(ifs, lp);
    }
    free(line_heap);
    return read_status;
}

int jash_b_read(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    return jash_b_read_from_fd(s, w, ctx, args, STDIN_FILENO);
}
