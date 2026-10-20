// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// Module: builtins/stat — file metadata, structured-JSON gold (ADR-0044).
//
// Why a builtin: `stat` is a workhorse agents call to inspect a file — size,
// type, mode, mtime. Its CLI is the WORST portability offender in coreutils:
// BSD stat (macOS) uses `-f '%z'` format soup, GNU stat uses `-c '%s'`, the
// default line differs on every platform, and NEITHER emits JSON. So an agent
// that stats a file has to brittle-parse a raw numeric line whose shape depends
// on the OS. The GOLD is `stat --json FILE...`: one portable, self-describing
// object per file — {name,type,size,mode,perms,nlink,uid,gid,mtime,...} — no
// format string, no BSD/GNU branch.
//
// Scope: this builtin OWNS only `stat --json [-L] [--] FILE...`. EVERY other
// invocation (default numeric line, -f/-c format strings, -x, -t, -s, -n, ...)
// falls back to the real stat, byte-for-byte — so nothing regresses. Default
// metadata source is lstat (a symlink reports type "symlink" + a "target"
// field); -L follows the link (stat). argv_fn (quote-aware).

#include "builtin.h"
#include <parser/exec.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <signal.h>
#include <core/json_writer.h>
#include <core/output_budget.h>
#include <parser/builtin_redir.h>
#include "query_data.h"

/* Reconstruct single-quoted argv and defer to the real stat (non-json path). */
static int stat_fallback(jash_state_t* s, jash_writer_t w, void* ctx,
                         int argc, char** argv) {
    size_t capacity = 1;
    for (int i = 0; i < argc; i++) {
        size_t n = strlen(argv[i]);
        if (capacity > SIZE_MAX - 3 || n > (SIZE_MAX - capacity - 3) / 4) return 1;
        capacity += n * 4 + 3;
    }
    char* args = malloc(capacity);
    if (!args) return 1;
    size_t o = 0;
    for (int i = 0; i < argc; i++) {
        if (i) args[o++] = ' ';
        args[o++] = '\'';
        for (const char* p = argv[i]; *p; p++) {
            if (*p == '\'') { args[o++]='\''; args[o++]='\\'; args[o++]='\''; args[o++]='\''; }
            else args[o++] = *p;
        }
        args[o++] = '\'';
    }
    args[o] = '\0';
    int rc = jash_exec_external(s, w, ctx, "stat", args);
    free(args); return rc;
}

static const char* stat_type_str(mode_t m) {
    if (S_ISREG(m))  return "file";
    if (S_ISDIR(m))  return "dir";
    if (S_ISLNK(m))  return "symlink";
    if (S_ISBLK(m))  return "block";
    if (S_ISCHR(m))  return "char";
    if (S_ISFIFO(m)) return "fifo";
    if (S_ISSOCK(m)) return "socket";
    return "unknown";
}

/* rwxrwxrwx symbolic permission string (9 chars + NUL). */
static void stat_perms(mode_t m, char out[10]) {
    static const char* rwx = "rwxrwxrwx";
    for (int i = 0; i < 9; i++) out[i] = (m & (mode_t)(0400 >> i)) ? rwx[i] : '-';
    out[9] = '\0';
}

typedef struct {
    struct stat metadata;
    int error_number;
    int target_error;
    size_t target_length;
    char target[4096];
} stat_observation_t;

/* JSON and typed values consume the same observations. Neither output adapter
 * performs a second metadata lookup or parses another adapter's text. */
static void stat_observe(const char* name, int follow, stat_observation_t* observed) {
    memset(observed, 0, sizeof(*observed));
    if ((follow ? stat(name, &observed->metadata) : lstat(name, &observed->metadata))) {
        observed->error_number = errno;
        return;
    }
    if (S_ISLNK(observed->metadata.st_mode)) {
        ssize_t length = readlink(name, observed->target, sizeof(observed->target));
        if (length < 0) observed->target_error = errno;
        else if ((size_t)length >= sizeof(observed->target)) observed->target_error = ENAMETOOLONG;
        else { observed->target_length = (size_t)length; observed->target[length] = '\0'; }
    }
}

/* JSON string with minimal escaping — names/targets can hold quotes/backslashes. */
static int stat_emit_obj(jash_json_writer_t* out, const char* name,
                          const stat_observation_t* observed) {
    const struct stat* st = &observed->metadata;
    char mode[16], perms[10];
    stat_perms(st->st_mode, perms);
    snprintf(mode, sizeof(mode), "%04o", (unsigned)(st->st_mode & 07777));
    jash_jsonw_raw(out, "{\"name\":"); jash_jsonw_string_or_bytes(out, name);
    jash_jsonw_raw(out, ",\"type\":"); jash_jsonw_string(out, stat_type_str(st->st_mode));
    jash_jsonw_raw(out, ",\"size\":"); jash_jsonw_safe_i64(out, st->st_size);
    jash_jsonw_raw(out, ",\"mode\":"); jash_jsonw_string(out, mode);
    jash_jsonw_raw(out, ",\"perms\":"); jash_jsonw_string(out, perms);
    jash_jsonw_raw(out, ",\"nlink\":"); jash_jsonw_safe_u64(out, st->st_nlink);
    jash_jsonw_raw(out, ",\"uid\":"); jash_jsonw_safe_u64(out, st->st_uid);
    jash_jsonw_raw(out, ",\"gid\":"); jash_jsonw_safe_u64(out, st->st_gid);
    jash_jsonw_raw(out, ",\"inode\":"); jash_jsonw_safe_u64(out, st->st_ino);
    jash_jsonw_raw(out, ",\"device\":"); jash_jsonw_safe_u64(out, st->st_dev);
    jash_jsonw_raw(out, ",\"mtime\":"); jash_jsonw_safe_i64(out, st->st_mtime);
    jash_jsonw_raw(out, ",\"atime\":"); jash_jsonw_safe_i64(out, st->st_atime);
    jash_jsonw_raw(out, ",\"ctime\":"); jash_jsonw_safe_i64(out, st->st_ctime);
#ifdef __APPLE__
    long ns[] = {st->st_mtimespec.tv_nsec, st->st_atimespec.tv_nsec, st->st_ctimespec.tv_nsec};
#else
    long ns[] = {st->st_mtim.tv_nsec, st->st_atim.tv_nsec, st->st_ctim.tv_nsec};
#endif
    jash_jsonw_raw(out, ",\"mtime_nsec\":"); jash_jsonw_i64(out, ns[0]);
    jash_jsonw_raw(out, ",\"atime_nsec\":"); jash_jsonw_i64(out, ns[1]);
    jash_jsonw_raw(out, ",\"ctime_nsec\":"); jash_jsonw_i64(out, ns[2]);
    int failed = 0;
    if (S_ISLNK(st->st_mode)) {                 /* only reachable without -L */
        jash_jsonw_raw(out, ",\"target\":");
        if (!observed->target_error) jash_jsonw_string_or_bytes(out, observed->target);
        else { jash_jsonw_raw(out, "null"); failed = 1; }
    }
    jash_jsonw_raw(out, "}"); return failed;
}

int jash_stat_json_mode(int argc, char* const* argv) {
    int mode = 0, data = 0;
    for (int i = 0; i < argc; i++) {
        const char* a = argv[i];
        if (!strcmp(a, "--") || a[0] != '-' || !a[1]) break;
        if (!strcmp(a, "--json")) mode = 1;
        else if (!strcmp(a, "--json-report")) mode = 2;
        else if (!strcmp(a, "--data") || !strcmp(a, "--data-v2")) data = 1;
        else if (!strcmp(a, "-c") || !strcmp(a, "-f") || !strcmp(a, "--format") ||
                 !strcmp(a, "--printf") || !strcmp(a, "--offset") || !strcmp(a, "--limit")) i++;
    }
    return data ? (mode ? -1 : 3) : mode;
}

static int stat_number(const char* value, uint64_t* out) {
    if (!value || !*value) return -1;
    uint64_t n = 0;
    for (; *value; value++) {
        if (*value < '0' || *value > '9' || n > (UINT64_MAX - (unsigned)(*value - '0')) / 10) return -1;
        n = n * 10 + (unsigned)(*value - '0');
    }
    *out = n; return 0;
}

QUERY_DATA_SCALAR(STAT_PATH, JDV_PATH, 0);
QUERY_DATA_SCALAR(STAT_NULL_PATH, JDV_PATH, 1);
QUERY_DATA_SCALAR(STAT_BOOL, JDV_BOOL, 0);
QUERY_DATA_SCALAR(STAT_U64, JDV_U64, 0);
QUERY_DATA_SCALAR(STAT_NULL_U64, JDV_U64, 1);
QUERY_DATA_SCALAR(STAT_NULL_I64, JDV_I64, 1);
QUERY_DATA_SCALAR(STAT_NULL_SIZE, JDV_FILESIZE, 1);
QUERY_DATA_SCALAR(STAT_NULL_TIME, JDV_TIMESTAMP, 1);
QUERY_DATA_SCALAR(STAT_NULL_STRING, JDV_STRING, 1);
QUERY_DATA_SCALAR(STAT_NULL_SYMBOL, JDV_SYMBOL, 1);

enum {
    ST_ATIME, ST_ATIME_NS, ST_ATIME_SECONDS, ST_COMPLETE,
    ST_CTIME, ST_CTIME_NS, ST_CTIME_SECONDS, ST_DEVICE, ST_ERRNO, ST_ERROR,
    ST_GID, ST_INODE, ST_MODE, ST_MTIME, ST_MTIME_NS, ST_MTIME_SECONDS,
    ST_NAME, ST_NLINK, ST_INDEX, ST_PERMS, ST_SIZE, ST_TARGET,
    ST_TIME_OVERFLOW, ST_TYPE, ST_UID, ST_FIELDS
};
static const jdv_schema_field_t STAT_DATA_FIELDS[] = {
    QUERY_DATA_FIELD("atime", &STAT_NULL_TIME),
    QUERY_DATA_FIELD("atime_nsec", &STAT_NULL_U64),
    QUERY_DATA_FIELD("atime_seconds", &STAT_NULL_I64),
    QUERY_DATA_FIELD("complete", &STAT_BOOL),
    QUERY_DATA_FIELD("ctime", &STAT_NULL_TIME),
    QUERY_DATA_FIELD("ctime_nsec", &STAT_NULL_U64),
    QUERY_DATA_FIELD("ctime_seconds", &STAT_NULL_I64),
    QUERY_DATA_FIELD("device", &STAT_NULL_U64),
    QUERY_DATA_FIELD("errno", &STAT_NULL_U64),
    QUERY_DATA_FIELD("error", &STAT_NULL_SYMBOL),
    QUERY_DATA_FIELD("gid", &STAT_NULL_U64),
    QUERY_DATA_FIELD("inode", &STAT_NULL_U64),
    QUERY_DATA_FIELD("mode", &STAT_NULL_U64),
    QUERY_DATA_FIELD("mtime", &STAT_NULL_TIME),
    QUERY_DATA_FIELD("mtime_nsec", &STAT_NULL_U64),
    QUERY_DATA_FIELD("mtime_seconds", &STAT_NULL_I64),
    QUERY_DATA_FIELD("name", &STAT_PATH),
    QUERY_DATA_FIELD("nlink", &STAT_NULL_U64),
    QUERY_DATA_FIELD("operand_index", &STAT_U64),
    QUERY_DATA_FIELD("perms", &STAT_NULL_STRING),
    QUERY_DATA_FIELD("size", &STAT_NULL_SIZE),
    QUERY_DATA_FIELD("target", &STAT_NULL_PATH),
    QUERY_DATA_FIELD("time_overflow", &STAT_BOOL),
    QUERY_DATA_FIELD("type", &STAT_NULL_SYMBOL),
    QUERY_DATA_FIELD("uid", &STAT_NULL_U64)
};
_Static_assert(sizeof(STAT_DATA_FIELDS) / sizeof(STAT_DATA_FIELDS[0]) == ST_FIELDS,
               "stat native field mapping");
static const jdv_schema_t STAT_DATA_SCHEMA = {
    .kind = JDV_SCHEMA_RECORD, .as.record = {STAT_DATA_FIELDS, ST_FIELDS, 0}
};

typedef struct {
    stat_observation_t observed;
    jdv_record_field_t fields[ST_FIELDS];
    char permissions[10];
} stat_data_row_t;

static int stat_data_item(stat_data_row_t* row, jdv_value_t* item,
                           const char* name, uint64_t index, int follow) {
    stat_observe(name, follow, &row->observed);
    *item = query_data_record(row->fields, &STAT_DATA_SCHEMA);
    jdv_record_field_t* f = row->fields;
    f[ST_NAME].value = query_data_text(JDV_PATH, name);
    f[ST_INDEX].value = jdv_value_u64(index);
    f[ST_TIME_OVERFLOW].value = jdv_value_bool(0);
    int failure = row->observed.error_number ? row->observed.error_number : row->observed.target_error;
    f[ST_COMPLETE].value = jdv_value_bool(!failure);
    if (failure) {
        f[ST_ERRNO].value = jdv_value_u64((uint64_t)failure);
        f[ST_ERROR].value = query_data_text(JDV_SYMBOL,
            row->observed.error_number ? "stat_failed" : "readlink_failed");
    }
    if (row->observed.error_number) return 1;
    const struct stat* st = &row->observed.metadata;
    f[ST_DEVICE].value = jdv_value_u64(st->st_dev);
    f[ST_GID].value = jdv_value_u64(st->st_gid);
    f[ST_INODE].value = jdv_value_u64(st->st_ino);
    f[ST_MODE].value = jdv_value_u64(st->st_mode & 07777);
    f[ST_NLINK].value = jdv_value_u64(st->st_nlink);
    f[ST_UID].value = jdv_value_u64(st->st_uid);
    f[ST_TYPE].value = query_data_text(JDV_SYMBOL, stat_type_str(st->st_mode));
    stat_perms(st->st_mode, row->permissions);
    f[ST_PERMS].value = query_data_text(JDV_STRING, row->permissions);
    if (st->st_size >= 0) f[ST_SIZE].value = jdv_value_filesize((uint64_t)st->st_size);
    else {
        f[ST_COMPLETE].value = jdv_value_bool(0);
        f[ST_ERRNO].value = jdv_value_u64(EOVERFLOW);
        f[ST_ERROR].value = query_data_text(JDV_SYMBOL, "negative_file_size");
        failure = EOVERFLOW;
    }
#ifdef __APPLE__
    const struct timespec times[] = {st->st_atimespec, st->st_ctimespec, st->st_mtimespec};
#else
    const struct timespec times[] = {st->st_atim, st->st_ctim, st->st_mtim};
#endif
    const int indices[] = {ST_ATIME, ST_CTIME, ST_MTIME};
    for (size_t i = 0; i < 3; i++) {
        int field = indices[i];
        f[field].value = query_data_timestamp(times[i].tv_sec, times[i].tv_nsec);
        f[field + 1].value = jdv_value_u64((uint64_t)times[i].tv_nsec);
        f[field + 2].value = jdv_value_i64(times[i].tv_sec);
        if (f[field].value.kind == JDV_NOTHING) f[ST_TIME_OVERFLOW].value = jdv_value_bool(1);
    }
    if (S_ISLNK(st->st_mode) && !row->observed.target_error)
        f[ST_TARGET].value = query_data_slice(JDV_PATH, row->observed.target,
                                             row->observed.target_length);
    return failure != 0;
}

static int stat_data_run(jash_state_t* state, jash_writer_t writer, void* context, int argc, char** argv) {
    int from = 0, follow = 0;
    uint64_t offset = 0, limit = UINT64_MAX;
    for (; from < argc; from++) {
        const char* arg = argv[from];
        if (!strcmp(arg, "--")) { from++; break; }
        if (arg[0] != '-' || !arg[1]) break;
        if (!strcmp(arg, "--data") || !strcmp(arg, "--data-v2")) continue;
        if (!strcmp(arg, "--offset") || !strcmp(arg, "--limit")) {
            int is_limit = !strcmp(arg, "--limit");
            if (++from >= argc || stat_number(argv[from], is_limit ? &limit : &offset) ||
                (is_limit && !limit))
                return query_data_error(writer, context, &STAT_DATA_SCHEMA, "invalid_page", 2);
            continue;
        }
        for (size_t j = 1; arg[j]; j++) {
            if (arg[j] != 'L')
                return query_data_error(writer, context, &STAT_DATA_SCHEMA, "unsupported_option", 2);
            follow = 1;
        }
    }
    if (from == argc)
        return query_data_error(writer, context, &STAT_DATA_SCHEMA, "missing_operand", 2);
    uint64_t available = (uint64_t)(argc - from);
    available = offset < available ? available - offset : 0;
    uint64_t selected = available < limit ? available : limit;
    size_t count = selected > QUERY_DATA_MAX_ITEMS ? QUERY_DATA_MAX_ITEMS : (size_t)selected;
    stat_data_row_t* rows = count ? calloc(count, sizeof(*rows)) : NULL;
    jdv_value_t* items = count ? calloc(count, sizeof(*items)) : NULL;
    if (count && (!rows || !items)) {
        free(rows); free(items);
        return query_data_error(writer, context, &STAT_DATA_SCHEMA, "allocation_failed", 1);
    }
    uint64_t errors = 0;
    size_t observed = 0;
    int cancelled = 0;
    for (size_t i = 0; i < count; i++) {
        if ((cancelled = jash_query_cancel_poll(state))) break;
        errors += (uint64_t)stat_data_item(&rows[i], &items[i],
            argv[from + (int)offset + (int)i], offset + i, follow);
        observed++;
    }
    int capped = selected > count;
    int incomplete = available > count;
    int status = cancelled ? cancelled : capped ? 75 : errors ? 1 : 0;
    int result = query_data_emit(writer, context, &STAT_DATA_SCHEMA, items, observed,
        errors, status, cancelled ? JDV_STREAM_CANCELLED : incomplete ? JDV_STREAM_INCOMPLETE : JDV_STREAM_COMPLETE,
        cancelled ? "request_cancelled" : capped ? "query_item_limit" : incomplete ? "page_limit" : NULL);
    free(rows); free(items);
    return result;
}

int jash_b_stat(jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv) {
    /* Pass 1: is --json requested (before any `--`)? If not, defer entirely to
     * the real stat so the default line / -f / -c / -x / -t stay byte-exact. */
    int json = jash_stat_json_mode(argc, argv);
    if (json < 0)
        return query_data_error(w, ctx, &STAT_DATA_SCHEMA, "incompatible_output_modes", 2);
    if (json == 3) return stat_data_run(s, w, ctx, argc, argv);
    if (!json) return stat_fallback(s, w, ctx, argc, argv);

    /* Pass 2: --json mode owns only -L / --json / -- / operands. Any other flag
     * is an error (the real stat has no --json to fall back onto). */
    int follow = 0, end_opts = 0, i = 0;
    uint64_t offset = 0, limit = UINT64_MAX;
    for (; i < argc; i++) {
        const char* a = argv[i];
        if (end_opts || a[0] != '-' || a[1] == '\0') break;   /* '-' alone → operand */
        if (strcmp(a, "--") == 0)     { i++; break; }
        if (!strcmp(a, "--json") || !strcmp(a, "--json-report")) continue;
        if (!strcmp(a, "--offset") || !strcmp(a, "--limit")) {
            int is_limit = !strcmp(a, "--limit");
            if (json != 2 || ++i >= argc || stat_number(argv[i], is_limit ? &limit : &offset) || (is_limit && !limit)) return 2;
            continue;
        }
        if (a[1] == '-') { jash_writef(jash_stderr_writer, 0, "stat: --json cannot combine with %s\n", a); return 1; }
        for (int c = 1; a[c]; c++) {
            if (a[c] == 'L') follow = 1;
            else { jash_writef(jash_stderr_writer, 0, "stat: --json cannot combine with -%c\n", a[c]); return 1; }
        }
    }

    if (i >= argc) { jash_writef(jash_stderr_writer, 0, "stat: --json: missing file operand\n"); return 1; }

    struct sigaction old_pipe, ignore = {0}; ignore.sa_handler = SIG_IGN; sigemptyset(&ignore.sa_mask);
    if (sigaction(SIGPIPE, &ignore, &old_pipe)) return 125;
    jash_budget_unwrap(&w, &ctx); jash_redir_stream_writer(w, ctx);
    jash_json_writer_t out; jash_jsonw_init(&out, w, ctx);
    int rc = 0, first = 1;
    uint64_t emitted = 0, failures = 0;
    jash_jsonw_raw(&out, json == 2 ? "{\"schema\":\"jash.query.stat.v1\",\"consistency\":\"live\",\"items\":[" : "[");
    for (int k = i; k < argc && !out.error && emitted < limit; k++) {
        if ((uint64_t)(k - i) < offset) continue;
        stat_observation_t observed;
        stat_observe(argv[k], follow, &observed);
        if (observed.error_number) {
            int saved = observed.error_number;
            jash_writef(jash_stderr_writer, 0, "stat: %s: %s\n", argv[k], strerror(saved));
            rc = 1; failures++;
            if (json == 2) {
                if (!first) jash_jsonw_raw(&out, ",");
                first = 0; emitted++;
                jash_jsonw_raw(&out, "{\"name\":"); jash_jsonw_string_or_bytes(&out, argv[k]);
                jash_jsonw_raw(&out, ",\"error\":\"stat_failed\",\"errno\":"); jash_jsonw_i64(&out, saved); jash_jsonw_raw(&out, "}");
            }
            continue;
        }
        if (!first) jash_jsonw_raw(&out, ",");
        first = 0;
        if (stat_emit_obj(&out, argv[k], &observed)) { rc = 1; failures++; }
        emitted++; jash_jsonw_finish(&out);
    }
    jash_jsonw_raw(&out, "]");
    if (json == 2) {
        jash_jsonw_raw(&out, ",\"count\":"); jash_jsonw_safe_u64(&out, emitted);
        jash_jsonw_raw(&out, ",\"failed\":"); jash_jsonw_safe_u64(&out, failures);
        jash_jsonw_raw(&out, ",\"next_offset\":");
        if (offset < (uint64_t)(argc - i) && emitted < (uint64_t)(argc - i) - offset) jash_jsonw_safe_u64(&out, offset + emitted);
        else jash_jsonw_raw(&out, "null");
        jash_jsonw_raw(&out, ",\"exit_code\":"); jash_jsonw_i64(&out, rc); jash_jsonw_raw(&out, "}");
    }
    jash_jsonw_raw(&out, "\n");
    if (jash_jsonw_finish(&out)) rc = 125;
    if (sigaction(SIGPIPE, &old_pipe, NULL)) rc = 125;
    return rc;
}
