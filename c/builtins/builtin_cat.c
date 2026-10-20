#define _GNU_SOURCE
// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// Module: builtins/cat — concatenate files, in-process (ADR-0044).
//
// ADR-0044 gold: --json emits [{path,size_bytes,lines}] objects for each file,
// enabling structured pipeline composition without regex-parsing prose output.

#include "builtin.h"
#include "query_data.h"
#include <core/code_highlight.h>
#include <core/agent_json.h>
#include <core/json_writer.h>
#include <core/structured_output.h>
#include <module/blake3.h>
#include <osif/osif_stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>

#define CAT_DATA_DEFAULT_LIMIT 4096u
#define CAT_DATA_MAX_LIMIT 16384u

static int cat_raw(FILE* f, jash_writer_t w, void* ctx) {
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) w(ctx, buf, n);
    return ferror(f) ? 1 : 0;
}

static int cat_is_c_source(const char* path) {
    const char* ext = path ? strrchr(path, '.') : NULL;
    return ext && (!strcasecmp(ext, ".c") || !strcasecmp(ext, ".h") ||
                   !strcasecmp(ext, ".cc") || !strcasecmp(ext, ".cpp") ||
                   !strcasecmp(ext, ".cxx") || !strcasecmp(ext, ".hpp"));
}

static int cat_highlight_c(FILE* file, jash_writer_t writer, void* context) {
    char* line = NULL;
    size_t capacity = 0;
    ssize_t read;
    jash_c_highlight_state_t state = {0};
    int failed = 0;
    while ((read = getline(&line, &capacity, file)) >= 0) {
        size_t length = (size_t)read;
        if (memchr(line, '\0', length)) {
            /* A binary-looking input is shown byte-for-byte, never tokenized. */
            if (writer(context, line, length) != (long)length) failed = 1;
            state.in_block_comment = 0;
            if (failed) break;
            continue;
        }
        size_t text_length = length;
        size_t newline_length = 0;
        if (text_length && line[text_length - 1] == '\n') {
            newline_length = 1;
            text_length--;
            if (text_length && line[text_length - 1] == '\r') {
                newline_length = 2;
                text_length--;
            }
        }
        if (jash_c_highlight_c_line(writer, context, line, text_length, &state) < 0) {
            failed = 1;
            break;
        }
        if (newline_length &&
            writer(context, line + text_length, newline_length) != (long)newline_length) {
            failed = 1;
            break;
        }
    }
    if (ferror(file)) failed = 1;
    free(line);
    return failed;
}

static void cat_emit_content(const char* p, size_t len, int show_tabs, int show_np, jash_writer_t w, void* ctx) {
    if (!show_tabs && !show_np) { w(ctx, p, len); return; }
    char out[8192]; size_t o = 0;
    #define CAT_PUT(ch) do { if (o >= sizeof out) { w(ctx,out,o); o=0; } out[o++]=(char)(ch); } while(0)
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)p[i];
        if (c == '\t') { if (show_tabs) { CAT_PUT('^'); CAT_PUT('I'); } else CAT_PUT('\t'); }
        else if (!show_np) CAT_PUT(c);
        else if (c < 32) { CAT_PUT('^'); CAT_PUT(c + 64); }
        else if (c == 127) { CAT_PUT('^'); CAT_PUT('?'); }
        else if (c >= 128) { CAT_PUT('M'); CAT_PUT('-'); if (c-128 < 32) { CAT_PUT('^'); CAT_PUT((c-128)+64); } else if (c-128 == 127) { CAT_PUT('^'); CAT_PUT('?'); } else CAT_PUT(c-128); }
        else CAT_PUT(c);
    }
    if (o) w(ctx, out, o);
    #undef CAT_PUT
}

static int cat_fmt(FILE* f, jash_writer_t w, void* ctx, int number, int number_nonblank, int squeeze, int show_ends, int show_tabs, int show_np, int* lineno, int* prev_blank, int* at_bol) {
    char buf[65536];
    while (fgets(buf, sizeof buf, f)) {
        size_t len = strlen(buf);
        int has_nl = (len > 0 && buf[len - 1] == '\n');
        if (has_nl) buf[--len] = '\0';
        if (*at_bol) {
            int is_blank = (len == 0) && has_nl;
            if (squeeze && is_blank && *prev_blank) { *prev_blank = 1; continue; }
            if (number_nonblank) { if (!is_blank) { char nb[16]; int k = snprintf(nb, sizeof nb, "%6d\t", ++(*lineno)); w(ctx, nb, (size_t)k); } }
            else if (number) { char nb[16]; int k = snprintf(nb, sizeof nb, "%6d\t", ++(*lineno)); w(ctx, nb, (size_t)k); }
            if (is_blank) *prev_blank = 1;
        }
        cat_emit_content(buf, len, show_tabs, show_np, w, ctx);
        if (has_nl) { if (show_ends) w(ctx, "$", 1); w(ctx, "\n", 1); *at_bol = 1; }
        else *at_bol = 0;
        if (*at_bol) {
            int is_blank = (len == 0);
            if (is_blank) *prev_blank = 1;
            else *prev_blank = 0;
        }
    }
    return ferror(f) ? 1 : 0;
}

static int cat_same_file(const struct stat* a, const struct stat* b) {
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino &&
           a->st_size == b->st_size &&
           BEAR_ST_MTIM(*a).tv_sec == BEAR_ST_MTIM(*b).tv_sec &&
           BEAR_ST_MTIM(*a).tv_nsec == BEAR_ST_MTIM(*b).tv_nsec &&
           BEAR_ST_CTIM(*a).tv_sec == BEAR_ST_CTIM(*b).tv_sec &&
           BEAR_ST_CTIM(*a).tv_nsec == BEAR_ST_CTIM(*b).tv_nsec;
}

static int cat_parse_u64(const char* text, uint64_t* out) {
    if (!text || !*text || !out) return -1;
    uint64_t value = 0;
    for (const unsigned char* p = (const unsigned char*)text; *p; p++) {
        if (*p < '0' || *p > '9') return -1;
        uint64_t digit = (uint64_t)(*p - '0');
        if (value > (UINT64_MAX - digit) / 10u) return -1;
        value = value * 10u + digit;
    }
    *out = value;
    return 0;
}

static void cat_json_error(jash_json_writer_t* json, const char* path,
                           int error) {
    jash_jsonw_raw(json, "{\"path\":");
    jash_jsonw_string_or_bytes(json, path);
    jash_jsonw_raw(json, ",\"error\":\"read_failed\",\"errno\":");
    jash_jsonw_i64(json, error);
    jash_jsonw_raw(json, "}");
}

/* Legacy --json metadata remains an array, but every path is byte-safe and
 * failures are represented as rows instead of malformed/silently empty JSON. */
static int cat_json_output(jash_json_writer_t* json, const char* path,
                           int is_stdin) {
    FILE* file = stdin;
    if (!is_stdin) {
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            int error = errno;
            cat_json_error(json, path, error);
            return 1;
        }
        file = fdopen(fd, "rb");
        if (!file) {
            int error = errno;
            close(fd);
            cat_json_error(json, path, error);
            return 1;
        }
    }
    if (!file) {
        int error = errno;
        cat_json_error(json, is_stdin ? "stdin" : path, error);
        return 1;
    }
    struct stat before = {0}, after = {0};
    uint64_t size_bytes = 0, lines = 0;
    int failed = 0, error = 0;
    if (!is_stdin) {
        if (fstat(fileno(file), &before) < 0) { failed = 1; error = errno; }
        else if (!S_ISREG(before.st_mode) || before.st_size < 0) {
            failed = 1; error = EINVAL;
        } else size_bytes = (uint64_t)before.st_size;
    }
    char buffer[65536];
    size_t count;
    while (!failed && (count = fread(buffer, 1, sizeof buffer, file)) > 0) {
        if (is_stdin) {
            if (size_bytes > UINT64_MAX - count) { failed = 1; error = EOVERFLOW; break; }
            size_bytes += count;
        }
        for (size_t i = 0; i < count; i++) if (buffer[i] == '\n') lines++;
    }
    if (!failed && ferror(file)) { failed = 1; error = errno ? errno : EIO; }
    if (!is_stdin) {
        if (!failed && (fstat(fileno(file), &after) < 0 || !cat_same_file(&before, &after))) {
            failed = 1; error = errno ? errno : ESTALE;
        }
        if (fclose(file) != 0 && !failed) { failed = 1; error = errno ? errno : EIO; }
    }
    if (failed) {
        cat_json_error(json, is_stdin ? "stdin" : path, error);
        return 1;
    }
    jash_jsonw_raw(json, "{\"path\":");
    jash_jsonw_string_or_bytes(json, is_stdin ? "stdin" : path);
    jash_jsonw_raw(json, ",\"size_bytes\":"); jash_jsonw_safe_u64(json, size_bytes);
    jash_jsonw_raw(json, ",\"lines\":"); jash_jsonw_safe_u64(json, lines);
    jash_jsonw_raw(json, "}");
    return 0;
}

static int cat_page_text_valid(const unsigned char* bytes, size_t length) {
    if (memchr(bytes, 0, length)) return 0;
    for (size_t offset = 0; offset < length;) {
        size_t width = jash_json_utf8_span(bytes + offset);
        if (!width || width > length - offset) return 0;
        offset += width;
    }
    return 1;
}

#include "query_snapshot.h"
typedef jash_query_file_page_t cat_page_t;

/* One observation supplies both the legacy JSON page and the native value.
 * Its bytes belong to the caller; no serializer reopens the named path. */
int jash_query_file_page(jash_state_t* state, const char* path,
                          uint64_t offset, size_t limit, jash_query_file_page_t* page) {
    memset(page, 0, sizeof(*page));
    int cancelled = jash_query_cancel_poll(state);
    if (cancelled) { page->error = "request_cancelled"; return cancelled; }
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        page->error = "open_failed"; page->error_number = errno;
        return 1;
    }
    struct stat before, after, named;
    int stat_rc = fstat(fd, &before);
    if (stat_rc < 0 || !S_ISREG(before.st_mode) || before.st_size < 0) {
        int saved = stat_rc < 0 ? errno : EINVAL;
        close(fd);
        page->error = stat_rc < 0 ? "stat_failed" : "non_regular_input";
        page->error_number = saved;
        return 1;
    }
    if (offset > (uint64_t)before.st_size) {
        close(fd);
        page->error = "offset_exceeds_file";
        return 2;
    }
    size_t amount = (size_t)((uint64_t)before.st_size - offset);
    if (amount > limit) amount = limit;
    unsigned char* bytes = malloc(amount + 1u);
    if (!bytes) { close(fd); page->error = "allocation_failed"; page->error_number = ENOMEM; return 1; }
    size_t used = 0;
    while (used < amount) {
        if ((cancelled = jash_query_cancel_poll(state))) {
            free(bytes); close(fd); page->error = "request_cancelled"; return cancelled;
        }
        ssize_t got = pread(fd, bytes + used, amount - used, (off_t)(offset + used));
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) {
            int saved = got < 0 ? errno : ESTALE;
            free(bytes); close(fd);
            page->error = "read_failed"; page->error_number = saved;
            return 1;
        }
        used += (size_t)got;
    }
    errno = 0;
    if (fstat(fd, &after) < 0 || stat(path, &named) < 0 ||
        !cat_same_file(&before, &after) || !cat_same_file(&before, &named)) {
        int saved = errno ? errno : ESTALE;
        free(bytes); close(fd);
        page->error = "changed_during_read"; page->error_number = saved;
        return 1;
    }
    if (close(fd) < 0) {
        int saved = errno;
        free(bytes);
        page->error = "close_failed"; page->error_number = saved;
        return 1;
    }
    if ((cancelled = jash_query_cancel_poll(state))) {
        free(bytes); page->error = "request_cancelled"; return cancelled;
    }
    bytes[used] = 0;
    page->bytes = bytes; page->used = used; page->metadata = before;
    blake3_hash(bytes, used, page->digest);
    return 0;
}

QUERY_DATA_SCALAR(CAT_PAGE_BOOL, JDV_BOOL, 0);
QUERY_DATA_SCALAR(CAT_PAGE_NULL_BOOL, JDV_BOOL, 1);
QUERY_DATA_SCALAR(CAT_PAGE_U64, JDV_U64, 0);
QUERY_DATA_SCALAR(CAT_PAGE_NULL_U64, JDV_U64, 1);
QUERY_DATA_SCALAR(CAT_PAGE_SIZE, JDV_FILESIZE, 0);
QUERY_DATA_SCALAR(CAT_PAGE_NULL_SIZE, JDV_FILESIZE, 1);
QUERY_DATA_SCALAR(CAT_PAGE_PATH, JDV_PATH, 0);
QUERY_DATA_SCALAR(CAT_PAGE_SYMBOL, JDV_SYMBOL, 0);
QUERY_DATA_SCALAR(CAT_PAGE_NULL_SYMBOL, JDV_SYMBOL, 1);
QUERY_DATA_SCALAR(CAT_PAGE_NULL_BYTES, JDV_BYTES, 1);
static const jdv_schema_field_t CAT_PAGE_FIELDS[] = {
    QUERY_DATA_FIELD("bytes_returned", &CAT_PAGE_SIZE),
    QUERY_DATA_FIELD("complete", &CAT_PAGE_BOOL),
    QUERY_DATA_FIELD("data", &CAT_PAGE_NULL_BYTES),
    QUERY_DATA_FIELD("device", &CAT_PAGE_NULL_U64),
    QUERY_DATA_FIELD("eof", &CAT_PAGE_NULL_BOOL),
    QUERY_DATA_FIELD("errno", &CAT_PAGE_NULL_U64),
    QUERY_DATA_FIELD("error", &CAT_PAGE_NULL_SYMBOL),
    QUERY_DATA_FIELD("freshness", &CAT_PAGE_SYMBOL),
    QUERY_DATA_FIELD("inode", &CAT_PAGE_NULL_U64),
    QUERY_DATA_FIELD("offset", &CAT_PAGE_U64),
    QUERY_DATA_FIELD("page_blake3", &CAT_PAGE_NULL_BYTES),
    QUERY_DATA_FIELD("path", &CAT_PAGE_PATH),
    QUERY_DATA_FIELD("total_bytes", &CAT_PAGE_NULL_SIZE)
};
static const jdv_schema_t CAT_PAGE_SCHEMA = {
    .kind = JDV_SCHEMA_RECORD, .as.record = {CAT_PAGE_FIELDS, 13, 0}
};

static int cat_page_native(jash_writer_t writer, void* context, const char* path,
                            uint64_t offset, const cat_page_t* page, int status) {
    jdv_record_field_t fields[13];
    jdv_value_t item = query_data_record(fields, &CAT_PAGE_SCHEMA);
    fields[0].value = jdv_value_filesize(page->used);
    fields[1].value = jdv_value_bool(!status);
    fields[7].value = query_data_text(JDV_SYMBOL, "descriptor-stable");
    fields[9].value = jdv_value_u64(offset);
    fields[11].value = query_data_text(JDV_PATH, path);
    int eof = !status && offset + page->used == (uint64_t)page->metadata.st_size;
    if (!status) {
        fields[2].value = query_data_slice(JDV_BYTES, page->bytes, page->used);
        fields[3].value = jdv_value_u64(page->metadata.st_dev);
        fields[4].value = jdv_value_bool(eof);
        fields[8].value = jdv_value_u64(page->metadata.st_ino);
        fields[10].value = query_data_slice(JDV_BYTES, page->digest, sizeof(page->digest));
        fields[12].value = jdv_value_filesize((uint64_t)page->metadata.st_size);
    } else {
        if (page->error_number) fields[5].value = jdv_value_u64((uint64_t)page->error_number);
        fields[6].value = query_data_text(JDV_SYMBOL, page->error ? page->error : "read_failed");
    }
    return query_data_emit(writer, context, &CAT_PAGE_SCHEMA, &item, 1,
        status && status < 128 ? 1 : 0, status,
        status >= 128 ? JDV_STREAM_CANCELLED : status ? JDV_STREAM_FAILED : eof ? JDV_STREAM_COMPLETE : JDV_STREAM_INCOMPLETE,
        status ? page->error : eof ? NULL : "page_limit");
}

static int cat_data_page(jash_state_t* state, jash_writer_t w, void* ctx, const char* path,
                         uint64_t offset, size_t limit, int native) {
    cat_page_t page;
    int status = jash_query_file_page(state, path, offset, limit, &page);
    if (native) {
        int result = cat_page_native(w, ctx, path, offset, &page, status);
        free(page.bytes);
        return result;
    }
    if (status) {
        jash_writef(jash_stderr_writer, NULL, "cat --data: %s: %s (%s)\n", path,
            page.error ? page.error : "read_failed", page.error_number ? strerror(page.error_number) : "invalid request");
        return status;
    }
    const unsigned char* bytes = page.bytes;
    size_t used = page.used;
    const struct stat before = page.metadata;
    char hash[65];
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof page.digest; i++) {
        hash[2u * i] = hex[page.digest[i] >> 4];
        hash[2u * i + 1u] = hex[page.digest[i] & 15u];
    }
    hash[64] = '\0';

    jash_structured_output_t output;
    if (jash_structured_begin(&output, w, ctx)) { free(page.bytes); return 125; }
    jash_json_writer_t* json = &output.json;
    jash_jsonw_raw(json, "{\"schema\":\"jash.file-page.v1\",\"operation\":\"file.read\",\"path\":");
    jash_jsonw_string_or_bytes(json, path);
    jash_jsonw_raw(json, ",\"offset\":"); jash_jsonw_safe_u64(json, offset);
    jash_jsonw_raw(json, ",\"bytes_returned\":"); jash_jsonw_safe_u64(json, used);
    jash_jsonw_raw(json, ",\"total_bytes\":"); jash_jsonw_safe_u64(json, (uint64_t)before.st_size);
    jash_jsonw_raw(json, ",\"eof\":");
    jash_jsonw_raw(json, offset + used == (uint64_t)before.st_size ? "true" : "false");
    jash_jsonw_raw(json, ",\"freshness\":\"descriptor-stable\",\"page_blake3\":");
    jash_jsonw_string(json, hash);
    jash_jsonw_raw(json, ",\"text\":");
    if (cat_page_text_valid(bytes, used)) jash_jsonw_string(json, (const char*)bytes);
    else jash_jsonw_raw(json, "null");
    jash_jsonw_raw(json, ",\"data\":"); jash_jsonw_bytes(json, bytes, used);
    jash_jsonw_raw(json, "}\n");
    int rc = jash_structured_finish(&output, 0);
    free(page.bytes);
    return rc;
}

int jash_b_cat(jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv) {
    int native_requested = 0;
    for (int n = 0; n < argc; n++) {
        const char* arg = argv[n];
        if (!strcmp(arg, "--") || arg[0] != '-' || !arg[1]) break;
        if (!strcmp(arg, "--data-v2")) native_requested = 1;
        else if (!strcmp(arg, "--offset") || !strcmp(arg, "--limit")) n++;
    }
#define CAT_ERROR(code, status) (native_requested ? query_data_error(w, ctx, &CAT_PAGE_SCHEMA, (code), (status)) : (status))
    int number = 0, number_nonblank = 0, squeeze = 0;
    int show_ends = 0, show_tabs = 0, show_np = 0;
    int color_mode = 0; /* 0=default/never, 1=auto, 2=always */
    int json_mode = 0, data_mode = 0, data_offset_given = 0, data_limit_given = 0;
    uint64_t data_offset = 0, data_limit = CAT_DATA_DEFAULT_LIMIT;
    
    int i = 0, end_opts = 0;
    for (; i < argc; i++) {
        const char* a = argv[i];
        if (end_opts || a[0] != '-' || a[1] == '\0') break;
        if (strcmp(a, "--") == 0) { i++; break; }
        if (!strcmp(a, "--number")) { number = 1; continue; }
        else if (!strcmp(a, "--number-nonblank")) { number_nonblank = 1; continue; }
        else if (!strcmp(a, "--squeeze-blank")) { squeeze = 1; continue; }
        else if (!strcmp(a, "--show-ends")) { show_ends = 1; continue; }
        else if (!strcmp(a, "--show-tabs")) { show_tabs = 1; continue; }
        else if (!strcmp(a, "--show-nonprinting")) { show_np = 1; continue; }
        else if (!strcmp(a, "--show-all")) { show_np = show_ends = show_tabs = 1; continue; }
        else if (!strcmp(a, "--highlight") || !strcmp(a, "--color") ||
                 !strcmp(a, "--color=auto")) { color_mode = 1; continue; }
        else if (!strcmp(a, "--color=always")) { color_mode = 2; continue; }
        else if (!strcmp(a, "--color=never")) { color_mode = 0; continue; }
        else if (!strcmp(a, "--json")) { json_mode = 1; continue; }
        else if (!strcmp(a, "--data")) { data_mode |= 1; continue; }
        else if (!strcmp(a, "--data-v2")) { data_mode |= 2; continue; }
        else if (!strcmp(a, "--offset") || !strcmp(a, "--limit")) {
            int is_offset = !strcmp(a, "--offset");
            uint64_t value;
            if (i + 1 >= argc || cat_parse_u64(argv[i + 1], &value) < 0) {
                jash_writef(jash_stderr_writer, NULL, "cat: %s requires a nonnegative decimal\n", a);
                return CAT_ERROR("invalid_page", 2);
            }
            i++;
            if (is_offset) { data_offset = value; data_offset_given = 1; }
            else { data_limit = value; data_limit_given = 1; }
            continue;
        } else if (!strncmp(a, "--offset=", 9) || !strncmp(a, "--limit=", 8)) {
            int is_offset = !strncmp(a, "--offset=", 9);
            const char* value_text = a + (is_offset ? 9 : 8);
            uint64_t value;
            if (cat_parse_u64(value_text, &value) < 0) {
                jash_writef(jash_stderr_writer, NULL, "cat: invalid %s value\n",
                            is_offset ? "--offset" : "--limit");
                return CAT_ERROR("invalid_page", 2);
            }
            if (is_offset) { data_offset = value; data_offset_given = 1; }
            else { data_limit = value; data_limit_given = 1; }
            continue;
        }
        else if (a[1] == '-') {
            jash_writef(jash_stderr_writer, 0, "cat: unknown option '%s'\n", a);
            return CAT_ERROR("unsupported_option", native_requested ? 2 : 1);
        }
        for (int j = 1; a[j]; j++) {
            switch (a[j]) {
                case 'n': number = 1; break;
                case 'b': number_nonblank = 1; break;
                case 's': squeeze = 1; break;
                case 'E': show_ends = 1; break;
                case 'T': show_tabs = 1; break;
                case 'v': show_np = 1; break;
                case 'e': show_np = show_ends = 1; break;
                case 't': show_np = show_tabs = 1; break;
                case 'A': show_np = show_ends = show_tabs = 1; break;
                case 'u': break;
                default:
                    jash_writef(jash_stderr_writer, 0, "cat: invalid option -- '%c'\n", a[j]);
                    return CAT_ERROR("unsupported_option", native_requested ? 2 : 1);
            }
        }
    }
    
    if (number_nonblank) number = 0;

    if (data_mode) {
        if (data_mode == 3 || json_mode || color_mode || number || number_nonblank || squeeze || show_ends ||
            show_tabs || show_np || argc - i != 1 || !strcmp(argv[i], "-")) {
            jash_writes(jash_stderr_writer, NULL,
                "usage: cat --data [--offset N] [--limit N] FILE (one regular file; limit 1..16384; no display flags)\n");
            return CAT_ERROR("incompatible_page_options", 2);
        }
        if (!data_limit_given) data_limit = CAT_DATA_DEFAULT_LIMIT;
        if (data_limit < 1 || data_limit > CAT_DATA_MAX_LIMIT) {
            jash_writes(jash_stderr_writer, NULL, "cat: --limit must be 1..16384\n");
            return CAT_ERROR("invalid_page", 2);
        }
        return cat_data_page(s, w, ctx, argv[i], data_offset, (size_t)data_limit, native_requested);
    }
    if (data_offset_given || data_limit_given) {
        jash_writes(jash_stderr_writer, NULL,
                    "cat: --offset/--limit require --data\n");
        return 2;
    }
    if (json_mode && (number || number_nonblank || squeeze || show_ends || show_tabs || show_np)) {
        jash_writes(jash_stderr_writer, NULL, "cat: --json cannot combine with display-format options\n");
        return 2;
    }
    if (color_mode && (json_mode || number || number_nonblank || squeeze ||
                       show_ends || show_tabs || show_np)) {
        jash_writes(jash_stderr_writer, NULL,
                    "cat: --color cannot combine with JSON or display-format options\n");
        return 2;
    }
    
    if (json_mode) {
        /* Metadata is structured output, not a prefix of human cat output. */
        jash_structured_output_t output;
        if (jash_structured_begin(&output, w, ctx)) return 125;
        jash_json_writer_t* json = &output.json;
        jash_jsonw_raw(json, "[");
        int rc = 0;
        int first = 1;
        
        if (i >= argc) {
            if (!first) jash_jsonw_raw(json, ",");
            first = 0;
            if (cat_json_output(json, "stdin", 1)) rc = 1;
        } else {
            for (int k = i; k < argc; k++) {
                if (!first) jash_jsonw_raw(json, ",");
                first = 0;
                if (cat_json_output(json, argv[k], strcmp(argv[k], "-") == 0)) rc = 1;
            }
        }
        jash_jsonw_raw(json, "]\n");
        return jash_structured_finish(&output, rc);
    }
    
    int formatted = number || number_nonblank || squeeze || show_ends || show_tabs || show_np;
    int lineno = 0, prev_blank = 0, at_bol = 1;
    int rc = 0;
    int any_operand = 0;
    
    for (int k = i; k < argc; k++) {
        any_operand = 1;
        FILE* f = (strcmp(argv[k], "-") == 0) ? stdin : fopen(argv[k], "rb");
        if (!f) { jash_writef(jash_stderr_writer, 0, "cat: %s: No such file or directory\n", argv[k]); rc = 1; continue; }
        int highlight = color_mode == 2 ||
            (color_mode == 1 && isatty(STDOUT_FILENO) && !getenv("NO_COLOR") &&
             (!getenv("TERM") || strcmp(getenv("TERM"), "dumb")) &&
             (!getenv("JASH_SENSORY") || strcmp(getenv("JASH_SENSORY"), "1")));
        int r = highlight && strcmp(argv[k], "-") && cat_is_c_source(argv[k])
            ? cat_highlight_c(f, w, ctx)
            : formatted ? cat_fmt(f, w, ctx, number, number_nonblank, squeeze,
                                  show_ends, show_tabs, show_np, &lineno,
                                  &prev_blank, &at_bol)
                        : cat_raw(f, w, ctx);
        if (r) rc = 1;
        if (strcmp(argv[k], "-") != 0) fclose(f);
    }
    
    if (!any_operand) {
        int r = formatted ? cat_fmt(stdin, w, ctx, number, number_nonblank, squeeze,
                                    show_ends, show_tabs, show_np, &lineno,
                                    &prev_blank, &at_bol)
                          : cat_raw(stdin, w, ctx);
        if (r) rc = 1;
    }
#undef CAT_ERROR
    return rc;
}
