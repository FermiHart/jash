// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// 🎵 Hear F E R M I ∞ H A R T music. No obligation. Just a wish.
//
// Module: builtins/df — Unix-standard df with unicode bars and colors
//
// Features:
//   df              — show filesystem usage with unicode bars
//   df -h           — human-readable sizes (K, M, G, T)
//   df /path        — show specific filesystem
//   df --color      — force colors (green < 70%, yellow 70-90%, red > 90%)
//   df --no-color   — disable colors
//
// Unicode Bars: █▓▒░ gradient showing disk usage
// Colors: green (safe), yellow (warning), red (critical)
// Pipe Detection: plain output when piped, colored when interactive

#include "builtin.h"
#include <core/env.h>
#include <core/argv.h>
#include <core/json_writer.h>
#include <core/structured_output.h>
#include <core/data_wire.h>
#include <core/timeline.h>
#include <core/output_budget.h>
#include <parser/builtin_redir.h>
#include <core/unix_format.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include "osif/osinfo.h"
#include <sys/statvfs.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <module/jash_crypto.h>
#include "query_data.h"

/* ── Color thresholds ─────────────────────────────────────────────────────── */

#define DF_GREEN_THRESHOLD  70   /* < 70% = green */
#define DF_YELLOW_THRESHOLD 90   /* 70-90% = yellow, > 90% = red */

/* ── ANSI color codes ─────────────────────────────────────────────────────── */

#define DF_COLOR_GREEN   "\033[32m"   /* safe */
#define DF_COLOR_YELLOW  "\033[33m"   /* warning */
#define DF_COLOR_RED     "\033[31m"   /* critical */
#define DF_COLOR_RESET   "\033[0m"
#define DF_COLOR_BOLD    "\033[1m"
#define DF_COLOR_DIM     "\033[2m"

/* ── Unicode bar characters ───────────────────────────────────────────────── */

#define DF_BAR_FULL    "█"
#define DF_BAR_DARK    "▓"
#define DF_BAR_MEDIUM  "▒"
#define DF_BAR_LIGHT   "░"
#define DF_BAR_EMPTY   " "

/* ── Filesystem info structure ────────────────────────────────────────────── */

typedef struct {
    char device[256];
    char mountpoint[256];
    char fstype[64];
    unsigned long long total;
    unsigned long long used;
    unsigned long long available;
    int use_percent;
    uint64_t filesystem_id;
    int identity_valid;
} df_fs_info_t;

/* ── Helper functions ─────────────────────────────────────────────────────── */

static int df_is_piped(void) {
    return !isatty(STDOUT_FILENO);
}

static void df_format_size(unsigned long long bytes, char* buf, size_t bufsz, int human) {
    if (!human) {
        snprintf(buf, bufsz, "%llu", bytes / 1024);  /* 1K blocks */
        return;
    }
    
    const char* units[] = {"B", "K", "M", "G", "T", "P"};
    int unit_idx = 0;
    double size = (double)bytes;
    
    while (size >= 1024.0 && unit_idx < 5) {
        size /= 1024.0;
        unit_idx++;
    }
    
    if (unit_idx == 0) {
        snprintf(buf, bufsz, "%llu%s", (unsigned long long)size, units[unit_idx]);
    } else {
        snprintf(buf, bufsz, "%.1f%s", size, units[unit_idx]);
    }
}

static const char* df_get_color(int use_percent, int piped, int force_color) {
    if (piped && !force_color) {
        return "";
    }
    
    if (use_percent < DF_GREEN_THRESHOLD) {
        return DF_COLOR_GREEN;
    } else if (use_percent < DF_YELLOW_THRESHOLD) {
        return DF_COLOR_YELLOW;
    } else {
        return DF_COLOR_RED;
    }
}

static void df_print_bar(int use_percent, int width, int piped, int force_color,
                         jash_writer_t w, void* ctx) {
    if (piped && !force_color) {
        /* Plain text percentage */
        char buf[16];
        snprintf(buf, sizeof(buf), "%3d%%", use_percent);
        jash_writes(w, ctx, buf);
        return;
    }
    
    int filled = (width * use_percent) / 100;
    const char* color = df_get_color(use_percent, piped, force_color);
    
    jash_writes(w, ctx, color);
    
    /* Gradient: full blocks for first 30%, then dark, medium, light */
    for (int i = 0; i < filled; i++) {
        double pos = (double)i / (double)filled;
        if (pos < 0.3) {
            jash_writes(w, ctx, DF_BAR_FULL);
        } else if (pos < 0.6) {
            jash_writes(w, ctx, DF_BAR_DARK);
        } else if (pos < 0.85) {
            jash_writes(w, ctx, DF_BAR_MEDIUM);
        } else {
            jash_writes(w, ctx, DF_BAR_LIGHT);
        }
    }
    
    /* Empty space */
    for (int i = filled; i < width; i++) {
        jash_writes(w, ctx, DF_BAR_EMPTY);
    }
    
    jash_writes(w, ctx, DF_COLOR_RESET);
    
    /* Percentage */
    char pct_buf[16];
    snprintf(pct_buf, sizeof(pct_buf), " %3d%%", use_percent);
    jash_writes(w, ctx, pct_buf);
}

/* ── Main df builtin ──────────────────────────────────────────────────────── */

/* Mount collection callback (C has no closures). */
typedef struct {
    df_fs_info_t* fs;
    int*          count;
    const char*   filter_path;
    int truncated;
} df_collect_ctx_t;

static int df_collect_cb(const osif_mount_t* m, void* user) {
    df_collect_ctx_t* c = (df_collect_ctx_t*)user;
    if (*c->count >= 64) { c->truncated = 1; return 1; }
    if (m->total_b == 0) return 0;
    /* NOTE: no filter here — `df PATH` needs the mount CONTAINING the path,
     * which is a longest-prefix decision made AFTER all mounts are known
     * (BUG-163). The old strncmp(mountpoint, path) filtered backwards. */
    df_fs_info_t* fs = &c->fs[(*c->count)++];
    memset(fs, 0, sizeof(*fs));
    strlcpy(fs->device,     m->device,     sizeof(fs->device));
    strlcpy(fs->mountpoint, m->mountpoint, sizeof(fs->mountpoint));
    strlcpy(fs->fstype,     m->fstype,     sizeof(fs->fstype));
    fs->total       = m->total_b;
    fs->used        = m->total_b - m->free_b;
    fs->available   = m->avail_b;
    fs->use_percent = (int)((long double)fs->used * 100 / fs->total);
    struct stat st;
    if (!stat(m->mountpoint, &st)) { fs->filesystem_id = st.st_dev; fs->identity_valid = 1; }
    return 0;
}

static int df_number(const char* text, uint64_t* result) {
    if (!text || !*text) return -1;
    uint64_t n = 0;
    for (; *text; text++) {
        if (*text < '0' || *text > '9' || n > (UINT64_MAX - (unsigned)(*text - '0')) / 10) return -1;
        n = n * 10 + (unsigned)(*text - '0');
    }
    *result = n; return 0;
}

typedef struct { char magic[8]; uint32_t count, row_size; unsigned char digest[32]; } df_baseline_header_t;
typedef struct {
    const char* destination;
    const char* error;
    uint64_t bytes_written;
    int published, durable, verified, error_number, cancelled;
    jash_recovery_ref_t undo;
} df_baseline_result_t;

static int df_io(int fd, void* data, size_t length, int writing) {
    unsigned char* p = data;
    while (length) {
        ssize_t n = writing ? write(fd, p, length) : read(fd, p, length);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n; length -= (size_t)n;
    }
    return 0;
}

static int df_baseline_read(const char* path, df_fs_info_t rows[64], int* count) {
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return -1;
    struct stat st; df_baseline_header_t header;
    int rc = -1;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 0077) ||
        df_io(fd, &header, sizeof(header), 0) || memcmp(header.magic, "JASHDF01", 8) ||
        header.count > 64 || header.row_size != sizeof(*rows) ||
        st.st_size != (off_t)(sizeof(header) + header.count * sizeof(*rows))) goto done;
    if (df_io(fd, rows, header.count * sizeof(*rows), 0)) goto done;
    unsigned char digest[32]; jash_sha256(rows, header.count * sizeof(*rows), digest);
    if (memcmp(digest, header.digest, 32)) goto done;
    for (uint32_t i = 0; i < header.count; i++)
        if (!memchr(rows[i].device, 0, sizeof(rows[i].device)) || !memchr(rows[i].mountpoint, 0, sizeof(rows[i].mountpoint)) ||
            !memchr(rows[i].fstype, 0, sizeof(rows[i].fstype)) || !rows[i].identity_valid) goto done;
    *count = (int)header.count; rc = 0;
done:
    if (close(fd)) rc = -1;
    return rc;
}

static int df_baseline_write(jash_state_t* state, const char* path, const df_fs_info_t* rows,
                              int count, df_baseline_result_t* result) {
    memset(result, 0, sizeof(*result)); result->destination = path;
    if (count < 1 || count > 64) { result->error = "invalid_baseline_rows"; return -1; }
    for (int i = 0; i < count; i++) if (!rows[i].identity_valid) {
        result->error = "filesystem_identity_unavailable"; return -1;
    }
    if ((result->cancelled = jash_query_cancel_poll(state))) {
        result->error = "request_cancelled"; return -1;
    }
    char* parent = strdup(path);
    if (!parent) { result->error = "allocation_failed"; result->error_number = ENOMEM; return -1; }
    char* slash = strrchr(parent, '/');
    const char* name = slash ? slash + 1 : path;
    if (!*name || !strcmp(name, ".") || !strcmp(name, "..")) {
        free(parent); result->error = "invalid_baseline_path"; return -1;
    }
    if (slash) *slash = 0;
    int dir = open(slash ? *parent ? parent : "/" : ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir < 0) {
        result->error_number = errno; result->error = "baseline_parent_open_failed"; free(parent); return -1;
    }
    static uint64_t sequence;
    char temporary[96];
    int fd = -1;
    for (unsigned attempt = 0; attempt < 128u && fd < 0; attempt++) {
        snprintf(temporary, sizeof(temporary), ".jash-df-%ld-%llu.tmp", (long)getpid(), (unsigned long long)++sequence);
        fd = openat(dir, temporary, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd < 0 && errno != EEXIST) break;
    }
    int rc = -1;
    if (fd >= 0) {
        struct stat owned;
        int have_owned = fstat(fd, &owned) == 0;
        df_baseline_header_t header = {0}; memcpy(header.magic, "JASHDF01", 8);
        header.count = (uint32_t)count; header.row_size = sizeof(*rows);
        jash_sha256(rows, (size_t)count * sizeof(*rows), header.digest);
        if (have_owned && !fchmod(fd, 0600) && !df_io(fd, &header, sizeof(header), 1) &&
            !df_io(fd, (void*)rows, (size_t)count * sizeof(*rows), 1) && !fsync(fd)) rc = 0;
        if (rc) { result->error = "baseline_write_failed"; result->error_number = errno ? errno : EIO; }
        if (!rc) {
            df_baseline_header_t check;
            df_fs_info_t checked_rows[64];
            unsigned char digest[32];
            if (lseek(fd, 0, SEEK_SET) < 0 || df_io(fd, &check, sizeof(check), 0) ||
                df_io(fd, checked_rows, (size_t)count * sizeof(*rows), 0)) {
                rc = -1; result->error = "baseline_verify_failed"; result->error_number = errno ? errno : EIO;
            } else {
                jash_sha256(checked_rows, (size_t)count * sizeof(*rows), digest);
                if (memcmp(&check, &header, sizeof(header)) || memcmp(digest, header.digest, sizeof(digest))) {
                    rc = -1; result->error = "baseline_verify_failed"; result->error_number = EIO;
                }
            }
        }
        if (!rc && (result->cancelled = jash_query_cancel_poll(state))) {
            rc = -1; result->error = "request_cancelled";
        }
        /* Publish the opened inode, not a temporary pathname another writer
         * may have replaced in a shared directory. No-replace final link. */
        if (!rc) {
#if defined(__linux__)
            char source_fd[64]; snprintf(source_fd, sizeof(source_fd), "/proc/self/fd/%d", fd);
            if (linkat(AT_FDCWD, source_fd, dir, name, AT_SYMLINK_FOLLOW)) {
                rc = -1; result->error = errno == EEXIST ? "baseline_exists" : "baseline_publish_failed";
                result->error_number = errno;
            } else {
                result->published = 1;
                result->bytes_written = sizeof(header) + (uint64_t)count * sizeof(*rows);
                struct stat published;
                if (fstatat(dir, name, &published, AT_SYMLINK_NOFOLLOW) ||
                    published.st_dev != owned.st_dev || published.st_ino != owned.st_ino ||
                    published.st_size != (off_t)result->bytes_written) {
                    rc = -1; result->error = "baseline_identity_changed"; result->error_number = errno ? errno : ESTALE;
                } else result->verified = 1;
            }
#else
            errno = ENOTSUP; rc = -1; result->error = "baseline_publish_unsupported"; result->error_number = ENOTSUP;
#endif
        }
        struct stat named;
        if (have_owned && !fstatat(dir, temporary, &named, AT_SYMLINK_NOFOLLOW) &&
            named.st_dev == owned.st_dev && named.st_ino == owned.st_ino) {
            if (unlinkat(dir, temporary, 0)) {
                rc = -1; if (!result->error) { result->error = "baseline_cleanup_failed"; result->error_number = errno; }
            }
        } else {
            rc = -1; if (!result->error) { result->error = "baseline_temporary_changed"; result->error_number = ESTALE; }
        }
        if (close(fd)) {
            rc = -1; if (!result->error) { result->error = "baseline_close_failed"; result->error_number = errno; }
        }
        if (result->published) {
            if (fsync(dir)) {
                rc = -1; if (!result->error) { result->error = "baseline_directory_sync_failed"; result->error_number = errno; }
            } else result->durable = 1;
        }
    } else {
        result->error = "baseline_temporary_open_failed"; result->error_number = errno;
    }
    if (close(dir)) {
        rc = -1; if (!result->error) { result->error = "baseline_parent_close_failed"; result->error_number = errno; }
    }
    free(parent);
    if (result->published && result->verified)
        (void)jash_timeline_record_ref(JASH_UNDO_FILE_CREATE, "df baseline create", path, NULL, &result->undo);
    if (!result->cancelled) result->cancelled = jash_query_cancel_poll(state);
    return rc;
}

static void df_delta(jash_json_writer_t* out, uint64_t now, uint64_t before) {
    if (now >= before) jash_jsonw_safe_u64(out, now - before);
    else if (before - now <= INT64_MAX) jash_jsonw_safe_i64(out, -(int64_t)(before - now));
    else { jash_jsonw_raw(out, "\"-"); jash_jsonw_u64(out, before - now); jash_jsonw_raw(out, "\""); }
}

static int df_json_item(jash_json_writer_t* out, const df_fs_info_t* fs, int inodes,
                        const df_fs_info_t* baseline, int delta) {
    jash_jsonw_raw(out, "{\"device\":"); jash_jsonw_string_or_bytes(out, fs->device);
    jash_jsonw_raw(out, ",\"mountpoint\":"); jash_jsonw_string_or_bytes(out, fs->mountpoint);
    jash_jsonw_raw(out, ",\"fstype\":"); jash_jsonw_string_or_bytes(out, fs->fstype);
    jash_jsonw_raw(out, ",\"total_bytes\":"); jash_jsonw_safe_u64(out, fs->total);
    jash_jsonw_raw(out, ",\"used_bytes\":"); jash_jsonw_safe_u64(out, fs->used);
    jash_jsonw_raw(out, ",\"avail_bytes\":"); jash_jsonw_safe_u64(out, fs->available);
    jash_jsonw_raw(out, ",\"use_percent\":"); jash_jsonw_i64(out, fs->use_percent);
    jash_jsonw_raw(out, ",\"filesystem_id\":");
    if (fs->identity_valid) jash_jsonw_safe_u64(out, fs->filesystem_id); else jash_jsonw_raw(out, "null");
    int failed = 0;
    if (delta) {
        jash_jsonw_raw(out, ",\"delta\":");
        if (!baseline) { jash_jsonw_raw(out, "null"); failed = 1; }
        else {
            jash_jsonw_raw(out, "{\"total_bytes\":"); df_delta(out, fs->total, baseline->total);
            jash_jsonw_raw(out, ",\"used_bytes\":"); df_delta(out, fs->used, baseline->used);
            jash_jsonw_raw(out, ",\"avail_bytes\":"); df_delta(out, fs->available, baseline->available);
            jash_jsonw_raw(out, "}");
        }
    }
    if (inodes) {
        struct statvfs st;
        if (statvfs(fs->mountpoint, &st)) { jash_jsonw_raw(out, ",\"inodes\":null"); failed = 1; }
        else {
            uint64_t used = st.f_files >= st.f_ffree ? st.f_files - st.f_ffree : 0;
            jash_jsonw_raw(out, ",\"inodes\":{\"total\":"); jash_jsonw_safe_u64(out, st.f_files);
            jash_jsonw_raw(out, ",\"used\":"); jash_jsonw_safe_u64(out, used);
            jash_jsonw_raw(out, ",\"free\":"); jash_jsonw_safe_u64(out, st.f_ffree);
            jash_jsonw_raw(out, ",\"available\":"); jash_jsonw_safe_u64(out, st.f_favail);
            jash_jsonw_raw(out, "}");
        }
    }
    jash_jsonw_raw(out, "}");
    return failed;
}

static int df_select_mount(const char* path, const df_fs_info_t* mounts, int count) {
    char resolved[PATH_MAX];
    if (!realpath(path, resolved)) return -1;
    int best = -1;
    size_t best_length = 0, length = strlen(resolved);
    for (int i = 0; i < count; i++) {
        size_t mount_length = strlen(mounts[i].mountpoint);
        if (mount_length <= length &&
            !strncmp(mounts[i].mountpoint, resolved, mount_length) &&
            (mount_length == 1 || resolved[mount_length] == '/' || !resolved[mount_length]) &&
            mount_length >= best_length) {
            best = i;
            best_length = mount_length;
        }
    }
    if (best < 0) errno = ENODEV;
    return best;
}

QUERY_DATA_SCALAR(DF_BOOL, JDV_BOOL, 0);
QUERY_DATA_SCALAR(DF_SIZE, JDV_FILESIZE, 0);
QUERY_DATA_SCALAR(DF_U64, JDV_U64, 0);
QUERY_DATA_SCALAR(DF_NULL_SIZE, JDV_FILESIZE, 1);
QUERY_DATA_SCALAR(DF_NULL_U64, JDV_U64, 1);
QUERY_DATA_SCALAR(DF_NULL_BYTES, JDV_BYTES, 1);
QUERY_DATA_SCALAR(DF_NULL_PATH, JDV_PATH, 1);
QUERY_DATA_SCALAR(DF_NULL_SYMBOL, JDV_SYMBOL, 1);
QUERY_DATA_SCALAR(DF_SYMBOL, JDV_SYMBOL, 0);
QUERY_DATA_SCALAR(DF_PATH, JDV_PATH, 0);
QUERY_DATA_SCALAR(DF_STRING, JDV_STRING, 0);
static const jdv_schema_field_t DF_UNDO_FIELDS[] = {
    QUERY_DATA_FIELD("coverage", &DF_SYMBOL),
    QUERY_DATA_FIELD("entry_id", &DF_U64),
    QUERY_DATA_FIELD("token", &DF_STRING)
};
static const jdv_schema_t DF_UNDO_SCHEMA = {
    .kind = JDV_SCHEMA_RECORD, .nullable = 1, .as.record = {DF_UNDO_FIELDS, 3, 0}
};

static const jdv_schema_field_t DF_BASELINE_FIELDS[] = {
    QUERY_DATA_FIELD("action", &DF_SYMBOL),
    QUERY_DATA_FIELD("bytes_written", &DF_SIZE),
    QUERY_DATA_FIELD("destination", &DF_PATH),
    QUERY_DATA_FIELD("durable", &DF_BOOL),
    QUERY_DATA_FIELD("effect", &DF_SYMBOL),
    QUERY_DATA_FIELD("errno", &DF_NULL_U64),
    QUERY_DATA_FIELD("error", &DF_NULL_SYMBOL),
    QUERY_DATA_FIELD("status", &DF_SYMBOL),
    QUERY_DATA_FIELD("undo", &DF_UNDO_SCHEMA),
    QUERY_DATA_FIELD("verified", &DF_BOOL)
};
static const jdv_schema_t DF_BASELINE_SCHEMA = {
    .kind = JDV_SCHEMA_RECORD, .nullable = 1, .as.record = {DF_BASELINE_FIELDS, 10, 0}
};

static const jdv_schema_field_t DF_DIFF_FIELDS[] = {
    QUERY_DATA_FIELD("magnitude", &DF_SIZE), QUERY_DATA_FIELD("negative", &DF_BOOL)
};
static const jdv_schema_t DF_DIFF_SCHEMA = {
    .kind = JDV_SCHEMA_RECORD, .as.record = {DF_DIFF_FIELDS, 2, 0}
};
static const jdv_schema_field_t DF_DELTA_FIELDS[] = {
    QUERY_DATA_FIELD("avail_bytes", &DF_DIFF_SCHEMA),
    QUERY_DATA_FIELD("total_bytes", &DF_DIFF_SCHEMA),
    QUERY_DATA_FIELD("used_bytes", &DF_DIFF_SCHEMA)
};
static const jdv_schema_t DF_DELTA_SCHEMA = {
    .kind = JDV_SCHEMA_RECORD, .nullable = 1, .as.record = {DF_DELTA_FIELDS, 3, 0}
};
static const jdv_schema_field_t DF_INODE_FIELDS[] = {
    QUERY_DATA_FIELD("available", &DF_U64), QUERY_DATA_FIELD("free", &DF_U64),
    QUERY_DATA_FIELD("total", &DF_U64), QUERY_DATA_FIELD("used", &DF_U64)
};
static const jdv_schema_t DF_INODE_SCHEMA = {
    .kind = JDV_SCHEMA_RECORD, .nullable = 1, .as.record = {DF_INODE_FIELDS, 4, 0}
};
enum {
    DF_AVAIL, DF_BASELINE, DF_BASELINE_ERROR, DF_COMPLETE, DF_DELTA, DF_DEVICE, DF_ERRNO,
    DF_ERROR, DF_ID, DF_TYPE, DF_INODES, DF_IS_TOTAL, DF_MOUNTPOINT,
    DF_OPERAND, DF_INDEX, DF_ROW_KIND, DF_TOTAL, DF_PERCENT, DF_USED, DF_FIELDS
};
static const jdv_schema_field_t DF_DATA_FIELDS[] = {
    QUERY_DATA_FIELD("avail_bytes", &DF_NULL_SIZE),
    QUERY_DATA_FIELD("baseline", &DF_BASELINE_SCHEMA),
    QUERY_DATA_FIELD("baseline_error", &DF_NULL_SYMBOL),
    QUERY_DATA_FIELD("complete", &DF_BOOL),
    QUERY_DATA_FIELD("delta", &DF_DELTA_SCHEMA),
    QUERY_DATA_FIELD("device", &DF_NULL_BYTES),
    QUERY_DATA_FIELD("errno", &DF_NULL_U64),
    QUERY_DATA_FIELD("error", &DF_NULL_SYMBOL),
    QUERY_DATA_FIELD("filesystem_id", &DF_NULL_U64),
    QUERY_DATA_FIELD("fstype", &DF_NULL_SYMBOL),
    QUERY_DATA_FIELD("inodes", &DF_INODE_SCHEMA),
    QUERY_DATA_FIELD("is_total", &DF_BOOL),
    QUERY_DATA_FIELD("mountpoint", &DF_NULL_PATH),
    QUERY_DATA_FIELD("operand", &DF_NULL_PATH),
    QUERY_DATA_FIELD("operand_index", &DF_NULL_U64),
    QUERY_DATA_FIELD("row_kind", &DF_SYMBOL),
    QUERY_DATA_FIELD("total_bytes", &DF_NULL_SIZE),
    QUERY_DATA_FIELD("use_percent", &DF_NULL_U64),
    QUERY_DATA_FIELD("used_bytes", &DF_NULL_SIZE)
};
_Static_assert(sizeof(DF_DATA_FIELDS) / sizeof(DF_DATA_FIELDS[0]) == DF_FIELDS,
               "df native field mapping");
static const jdv_schema_t DF_DATA_SCHEMA = {
    .kind = JDV_SCHEMA_RECORD, .as.record = {DF_DATA_FIELDS, DF_FIELDS, 0}
};
typedef struct {
    jdv_record_field_t fields[DF_FIELDS];
    jdv_record_field_t inodes[4];
    jdv_record_field_t delta[3];
    jdv_record_field_t differences[3][2];
    jdv_record_field_t baseline[10];
    jdv_record_field_t undo[3];
} df_data_row_t;

static jdv_value_t df_data_difference(jdv_record_field_t fields[2],
                                       uint64_t current, uint64_t previous) {
    jdv_value_t value = query_data_record(fields, &DF_DIFF_SCHEMA);
    fields[0].value = jdv_value_filesize(current >= previous ? current - previous : previous - current);
    fields[1].value = jdv_value_bool(current < previous);
    return value;
}

static jdv_value_t df_data_item(df_data_row_t* row, const df_fs_info_t* fs,
                                 const char* operand, uint64_t index, int total,
                                 int inodes, const df_fs_info_t* previous,
                                 int delta, const char* baseline_error,
                                 const char* error, int error_number) {
    jdv_value_t item = query_data_record(row->fields, &DF_DATA_SCHEMA);
    jdv_record_field_t* f = row->fields;
    f[DF_COMPLETE].value = jdv_value_bool(!error && !baseline_error);
    f[DF_IS_TOTAL].value = jdv_value_bool(total);
    f[DF_ROW_KIND].value = query_data_text(JDV_SYMBOL, total ? "total" : "filesystem");
    if (!total) f[DF_INDEX].value = jdv_value_u64(index);
    if (operand) f[DF_OPERAND].value = query_data_text(JDV_PATH, operand);
    if (baseline_error) f[DF_BASELINE_ERROR].value = query_data_text(JDV_SYMBOL, baseline_error);
    if (error) f[DF_ERROR].value = query_data_text(JDV_SYMBOL, error);
    if (error_number) f[DF_ERRNO].value = jdv_value_u64((uint64_t)error_number);
    if (!fs) return item;
    f[DF_AVAIL].value = jdv_value_filesize(fs->available);
    f[DF_TOTAL].value = jdv_value_filesize(fs->total);
    f[DF_USED].value = jdv_value_filesize(fs->used);
    f[DF_PERCENT].value = jdv_value_u64((uint64_t)fs->use_percent);
    if (!total) {
        f[DF_DEVICE].value = query_data_text(JDV_BYTES, fs->device);
        f[DF_MOUNTPOINT].value = query_data_text(JDV_PATH, fs->mountpoint);
        f[DF_TYPE].value = query_data_text(JDV_SYMBOL, fs->fstype);
        if (fs->identity_valid) f[DF_ID].value = jdv_value_u64(fs->filesystem_id);
    }
    if (inodes && !total) {
        struct statvfs info;
        if (statvfs(fs->mountpoint, &info)) {
            f[DF_COMPLETE].value = jdv_value_bool(0);
            f[DF_ERROR].value = query_data_text(JDV_SYMBOL, "inodes_unavailable");
            f[DF_ERRNO].value = jdv_value_u64((uint64_t)(errno ? errno : EIO));
        } else {
            f[DF_INODES].value = query_data_record(row->inodes, &DF_INODE_SCHEMA);
            row->inodes[0].value = jdv_value_u64(info.f_favail);
            row->inodes[1].value = jdv_value_u64(info.f_ffree);
            row->inodes[2].value = jdv_value_u64(info.f_files);
            row->inodes[3].value = jdv_value_u64(info.f_files >= info.f_ffree ? info.f_files - info.f_ffree : 0);
        }
    }
    if (delta && !total && previous) {
        f[DF_DELTA].value = query_data_record(row->delta, &DF_DELTA_SCHEMA);
        row->delta[0].value = df_data_difference(row->differences[0], fs->available, previous->available);
        row->delta[1].value = df_data_difference(row->differences[1], fs->total, previous->total);
        row->delta[2].value = df_data_difference(row->differences[2], fs->used, previous->used);
    }
    return item;
}

#include "df_baseline_data.h"

static int df_data_run(jash_state_t* state, jash_writer_t writer, void* context,
                        const df_fs_info_t* mounts, int mount_count, int truncated,
                        const char* const* operands, int operand_count,
                        int inodes, int grand_total, uint64_t offset, uint64_t limit,
                        const char* baseline_path, const char* create_path) {
    size_t all = operand_count ? (size_t)operand_count : (size_t)mount_count;
    if (all > QUERY_DATA_MAX_ITEMS)
        return query_data_error(writer, context, &DF_DATA_SCHEMA, "query_operand_limit", 75);
    size_t remaining = offset < all ? all - (size_t)offset : 0;
    size_t count = remaining < limit ? remaining : (size_t)limit;
    size_t capacity = count + (grand_total != 0) + (create_path != NULL);
    df_data_row_t* rows = capacity ? calloc(capacity, sizeof(*rows)) : NULL;
    jdv_value_t* items = capacity ? calloc(capacity, sizeof(*items)) : NULL;
    if (capacity && (!rows || !items)) {
        free(rows); free(items);
        return query_data_error(writer, context, &DF_DATA_SCHEMA, "allocation_failed", 1);
    }
    df_fs_info_t baseline[64];
    df_fs_info_t publication[64];
    unsigned char publication_seen[64] = {0};
    int publication_count = 0;
    int baseline_count = 0;
    const char* baseline_error = NULL;
    if (baseline_path && df_baseline_read(baseline_path, baseline, &baseline_count))
        baseline_error = "invalid_baseline";
    uint64_t errors = baseline_error ? 1 : 0;
    df_fs_info_t total = {0};
    int total_overflow = 0;
    int cancelled = 0;
    size_t emitted = 0;
    for (size_t i = 0; i < all; i++) {
        if ((cancelled = jash_query_cancel_poll(state))) break;
        const char* operand = operand_count ? operands[i] : NULL;
        int selected = operand_count ? df_select_mount(operand, mounts, mount_count) : (int)i;
        int lookup_error = selected < 0 ? errno : 0;
        const df_fs_info_t* fs = selected >= 0 ? &mounts[selected] : NULL;
        const char* error = selected < 0 ? "filesystem_lookup_failed" : NULL;
        if (!fs) errors++;
        const df_fs_info_t* previous = NULL;
        if (fs && baseline_path && !baseline_error) {
            for (int b = 0; b < baseline_count; b++)
                if (fs->identity_valid && baseline[b].filesystem_id == fs->filesystem_id &&
                    !strcmp(baseline[b].device, fs->device) &&
                    !strcmp(baseline[b].mountpoint, fs->mountpoint) &&
                    !strcmp(baseline[b].fstype, fs->fstype)) {
                    previous = &baseline[b]; break;
                }
            if (!previous) { error = "baseline_mismatch"; errors++; }
        }
        if (fs) {
            if (create_path && !publication_seen[selected]) {
                publication_seen[selected] = 1;
                publication[publication_count++] = *fs;
            }
            if (UINT64_MAX - total.total < fs->total || UINT64_MAX - total.used < fs->used ||
                UINT64_MAX - total.available < fs->available) total_overflow = 1;
            else { total.total += fs->total; total.used += fs->used; total.available += fs->available; }
        }
        if (i < offset || emitted >= count) continue;
        items[emitted] = df_data_item(&rows[emitted], fs, operand, i, 0, inodes,
                                      previous, baseline_path != NULL, baseline_error,
                                      error, lookup_error);
        if (!error && rows[emitted].fields[DF_ERROR].value.kind != JDV_NOTHING) errors++;
        emitted++;
    }
    if (grand_total) {
        if (total_overflow) errors++;
        total.use_percent = total.total ? (int)((long double)total.used * 100 / total.total) : 0;
        items[emitted] = df_data_item(&rows[emitted], total_overflow ? NULL : &total,
            NULL, 0, 1, 0, NULL, 0, NULL,
            total_overflow ? "total_overflow" : cancelled ? "cancelled" : errors ? "partial" : NULL,
            total_overflow ? EOVERFLOW : 0);
        emitted++;
    }
    int incomplete = truncated || remaining > count;
    int result;
    if (create_path) result = df_baseline_data_publish(state, writer, context, rows, items, emitted,
        errors, truncated, remaining > count, cancelled, create_path, publication, publication_count);
    else result = query_data_emit(writer, context, &DF_DATA_SCHEMA, items, emitted,
            errors, cancelled ? cancelled : truncated ? 75 : errors ? 1 : 0,
            cancelled ? JDV_STREAM_CANCELLED : incomplete ? JDV_STREAM_INCOMPLETE : JDV_STREAM_COMPLETE,
            cancelled ? "request_cancelled" : truncated ? "mount_collection_limit" : incomplete ? "page_limit" : NULL);
    free(rows); free(items);
    return result;
}

static int df_data_mode(int argc, char** argv) {
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--")) break;
        if (!strcmp(argv[i], "--data") || !strcmp(argv[i], "--data-v2")) return 1;
        if (!strcmp(argv[i], "--baseline") || !strcmp(argv[i], "--delta") ||
            !strcmp(argv[i], "--offset") || !strcmp(argv[i], "--limit")) i++;
    }
    return 0;
}

static int df_run(jash_state_t* state, jash_writer_t w, void* ctx, int argc, char** argv,
                  const char** paths, df_fs_info_t* selected) {
    
    /* Parse arguments */
    int human_readable = 0;
    int force_color = 0;
    int no_color = 0;
    int inodes = 0;
    int json_out = 0;
    int data_out = df_data_mode(argc, argv);
    int show_type = 0;   /* -T */
    int grand_total = 0; /* --total */
    int path_count = 0;
    uint64_t offset = 0, limit = UINT64_MAX;
    const char* baseline_path = NULL;
    int delta_mode = 0;
    {
        int end_opts = 0;
        for (int i = 0; i < argc; i++) {
            if (!end_opts && strcmp(argv[i], "--") == 0) { end_opts = 1; continue; }
            if (!end_opts && (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--human-readable") == 0)) {
                human_readable = 1;
            } else if (!end_opts && (strcmp(argv[i], "--color") == 0 || strcmp(argv[i], "--color=always") == 0)) {
                force_color = 1;
            } else if (!end_opts && strcmp(argv[i], "--no-color") == 0) {
                no_color = 1;
            } else if (!end_opts && (strcmp(argv[i], "-i") == 0 || strcmp(argv[i], "--inodes") == 0)) {
                inodes = 1;
            } else if (!end_opts && (strcmp(argv[i], "-T") == 0 || strcmp(argv[i], "--print-type") == 0)) {
                show_type = 1;
            } else if (!end_opts && strcmp(argv[i], "--total") == 0) {
                grand_total = 1;
            } else if (!end_opts && strcmp(argv[i], "--json") == 0) {
                json_out = 1;
            } else if (!end_opts && strcmp(argv[i], "--json-report") == 0) {
                json_out = 2;
            } else if (!end_opts && (!strcmp(argv[i], "--data") || !strcmp(argv[i], "--data-v2"))) {
                data_out = 1;
            } else if (!end_opts && (!strcmp(argv[i], "--baseline") || !strcmp(argv[i], "--delta"))) {
                if (baseline_path || i + 1 >= argc)
                    return data_out ? query_data_error(w, ctx, &DF_DATA_SCHEMA, "invalid_baseline_option", 2) : 2;
                delta_mode = !strcmp(argv[i], "--delta"); baseline_path = argv[++i];
            } else if (!end_opts && (!strcmp(argv[i], "--offset") || !strcmp(argv[i], "--limit"))) {
                int is_limit = !strcmp(argv[i], "--limit");
                if (++i >= argc || df_number(argv[i], is_limit ? &limit : &offset) || (is_limit && !limit))
                    return data_out ? query_data_error(w, ctx, &DF_DATA_SCHEMA, "invalid_page", 2) : 2;
            } else if (!end_opts && (strcmp(argv[i], "-H") == 0 || strcmp(argv[i], "--si") == 0)) {
                human_readable = 1;   /* approximate: SI ≈ human */
            } else if (!end_opts && strcmp(argv[i], "-k") == 0) {
                /* 1K blocks — the default; accept for parity. */
            } else if (!end_opts && (strcmp(argv[i], "-P") == 0 || strcmp(argv[i], "--portability") == 0)) {
                /* POSIX output — our text is already close; accept. */
            } else if (!end_opts && argv[i][0] == '-' && argv[i][1] != '\0') {
                if (data_out) return query_data_error(w, ctx, &DF_DATA_SCHEMA, "unsupported_option", 2);
                jash_writef(jash_stderr_writer, 0, "df: unknown option '%s'\n", argv[i]);
                return 1;
            } else {
                paths[path_count++] = argv[i];
            }
        }
    }
    if (data_out && json_out)
        return query_data_error(w, ctx, &DF_DATA_SCHEMA, "incompatible_output_modes", 2);
    if ((offset || limit != UINT64_MAX) && json_out != 2 && !data_out) return 2;
    if (baseline_path && json_out != 2 && !data_out) return 2;
    int cancelled = data_out ? jash_query_cancel_poll(state) : 0;
    if (cancelled)
        return query_data_emit(w, ctx, &DF_DATA_SCHEMA, NULL, 0, 0, cancelled,
                               JDV_STREAM_CANCELLED, "request_cancelled");
    
    int piped = df_is_piped();
    if (no_color) {
        force_color = 0;
    }
    
    df_fs_info_t collected[64];
    df_fs_info_t* filesystems = collected;
    int fs_count = 0;

    /* ADR-0017 wave 2: ONE mount enumerator for every OS via OSIF.
     * Backends filter pseudo-filesystems; this file is now OS-agnostic. */
    if (!(bear_osinfo_caps() & OSIF_CAP_MOUNTS)) {
        if (data_out) return query_data_error(w, ctx, &DF_DATA_SCHEMA, "mount_info_unavailable", 1);
        if (json_out)
            return jash_structured_error(w, ctx, "df", "mount_info_unavailable", 1);
        jash_writes(w, ctx,
            "df: mount info not available on this OS (OSIF_CAP_MOUNTS off)\n");
        return 1;
    }

    df_collect_ctx_t cctx = { filesystems, &fs_count, NULL, 0 };
    if (bear_osinfo_mounts(df_collect_cb, &cctx) != 0) {
        if (data_out) return query_data_error(w, ctx, &DF_DATA_SCHEMA, "mount_table_read_failed", 1);
        if (json_out)
            return jash_structured_error(w, ctx, "df", "mount_table_read_failed", 1);
        jash_writes(w, ctx, "df: cannot read mount table\n");
        return 1;
    }
    if (data_out)
        return df_data_run(state, w, ctx, filesystems, fs_count, cctx.truncated,
                            paths, path_count, inodes, grand_total, offset, limit,
                            delta_mode ? baseline_path : NULL, delta_mode ? NULL : baseline_path);

    /* BUG-163: `df PATH` reports the filesystem CONTAINING path — the mount
     * whose mountpoint is the longest prefix of the resolved path. */
    int errors = 0;
    int selected_count = 0;
    for (int p = 0; p < path_count; p++) {
        const char* filter_path = paths[p];
        int best = df_select_mount(filter_path, filesystems, fs_count);
        if (best < 0) {
            jash_writef(jash_stderr_writer, 0, "df: %s: No such file or directory\n", filter_path);
            errors = 1;
            continue;
        }
        selected[selected_count++] = filesystems[best];
    }
    if (path_count) {
        filesystems = selected;
        fs_count = selected_count;
    }

    /* --json: one JSON object per filesystem, isolated from the text path. */
    if (json_out) {
        struct sigaction old_pipe, ignore_pipe = {0};
        ignore_pipe.sa_handler = SIG_IGN; sigemptyset(&ignore_pipe.sa_mask);
        if (sigaction(SIGPIPE, &ignore_pipe, &old_pipe)) return 125;
        jash_budget_unwrap(&w, &ctx); jash_redir_stream_writer(w, ctx);
        jash_json_writer_t out; jash_jsonw_init(&out, w, ctx);
        uint64_t emitted = 0;
        df_fs_info_t total = {0};
        strlcpy(total.device, "total", sizeof(total.device));
        strlcpy(total.mountpoint, "-", sizeof(total.mountpoint));
        if (json_out == 2) jash_jsonw_raw(&out, "{\"schema\":\"jash.query.df.v1\",\"consistency\":\"live\",\"items\":[");
        df_fs_info_t baseline[64]; int baseline_count = 0;
        const char* baseline_error = NULL;
        df_baseline_result_t baseline_result = {.destination = baseline_path};
        if (baseline_path && !jash_jsonw_finish(&out)) {
            if (delta_mode) {
                if (df_baseline_read(baseline_path, baseline, &baseline_count)) { baseline_error = "invalid_baseline"; errors = 1; }
            } else {
                if (errors || cctx.truncated) {
                    baseline_result.error = cctx.truncated ? "mount_collection_limit" : "baseline_observation_failed";
                } else (void)df_baseline_write(state, baseline_path, filesystems, fs_count, &baseline_result);
                if (baseline_result.error) { baseline_error = baseline_result.error; errors = 1; }
                if (baseline_result.cancelled) errors = baseline_result.cancelled;
            }
        }
        for (int i = 0; i < fs_count && !out.error; i++) {
            df_fs_info_t* fs = &filesystems[i];
            if (UINT64_MAX - total.total < fs->total || UINT64_MAX - total.used < fs->used ||
                UINT64_MAX - total.available < fs->available) { errors = 1; grand_total = 0; }
            else { total.total += fs->total; total.used += fs->used; total.available += fs->available; }
            if ((uint64_t)i < offset || emitted >= limit) continue;
            if (json_out == 2 && emitted) jash_jsonw_raw(&out, ",");
            const df_fs_info_t* previous = NULL;
            if (delta_mode && fs->identity_valid) for (int b = 0; b < baseline_count; b++)
                if (baseline[b].filesystem_id == fs->filesystem_id && !strcmp(baseline[b].device, fs->device) &&
                    !strcmp(baseline[b].mountpoint, fs->mountpoint) && !strcmp(baseline[b].fstype, fs->fstype)) {
                    previous = &baseline[b]; break;
                }
            errors |= df_json_item(&out, fs, inodes, previous, delta_mode);
            emitted++;
            if (json_out == 1) jash_jsonw_raw(&out, "\n");
            jash_jsonw_finish(&out);
        }
        if (grand_total) total.use_percent = total.total ? (int)((long double)total.used * 100 / total.total) : 0;
        if (json_out == 2) {
            jash_jsonw_raw(&out, "],\"total\":");
            if (grand_total) df_json_item(&out, &total, 0, NULL, 0); else jash_jsonw_raw(&out, "null");
            if (baseline_path) {
                jash_jsonw_raw(&out, ",\"baseline_mode\":"); jash_jsonw_string(&out, delta_mode ? "compare" : "create");
                jash_jsonw_raw(&out, ",\"baseline_error\":");
                if (baseline_error) jash_jsonw_string(&out, baseline_error); else jash_jsonw_raw(&out, "null");
                if (!delta_mode) {
                    jash_jsonw_raw(&out, ",\"baseline_receipt\":"); df_baseline_json(&out, &baseline_result);
                }
            }
            jash_jsonw_raw(&out, ",\"count\":"); jash_jsonw_safe_u64(&out, emitted);
            jash_jsonw_raw(&out, ",\"next_offset\":");
            if (offset < (uint64_t)fs_count && emitted < (uint64_t)fs_count - offset) jash_jsonw_safe_u64(&out, offset + emitted);
            else jash_jsonw_raw(&out, "null");
            jash_jsonw_raw(&out, ",\"collection_truncated\":"); jash_jsonw_raw(&out, cctx.truncated ? "true" : "false");
            jash_jsonw_raw(&out, ",\"exit_code\":"); jash_jsonw_i64(&out, errors);
            jash_jsonw_raw(&out, "}\n");
        } else if (grand_total) {
            df_json_item(&out, &total, 0, NULL, 0); jash_jsonw_raw(&out, "\n");
        }
        int rc = jash_jsonw_finish(&out) ? 125 : errors;
        if (sigaction(SIGPIPE, &old_pipe, NULL)) rc = 125;
        return rc;
    }

    /* Print header */
    if (!piped || force_color) {
        jash_writes(w, ctx, DF_COLOR_BOLD);
    }
    
    if (inodes) {
        jash_writes(w, ctx, "Filesystem               Inodes     IUsed     IFree IUse% Mounted on\n");
    } else if (human_readable) {
        if (show_type) jash_writes(w, ctx, "Filesystem      Type      Size  Used  Avail Use% Mounted on\n");
        else           jash_writes(w, ctx, "Filesystem      Size  Used  Avail Use% Mounted on\n");
    } else {
        if (show_type) jash_writes(w, ctx, "Filesystem     Type      1K-blocks      Used Available Use% Mounted on\n");
        else           jash_writes(w, ctx, "Filesystem     1K-blocks      Used Available Use% Mounted on\n");
    }
    
    if (!piped || force_color) {
        jash_writes(w, ctx, DF_COLOR_RESET);
    }
    
    /* Print filesystems */
    unsigned long long tot_total = 0, tot_used = 0, tot_avail = 0;
    for (int i = 0; i < fs_count; i++) {
        df_fs_info_t* fs = &filesystems[i];
        tot_total += fs->total; tot_used += fs->used; tot_avail += fs->available;

        char total_str[32], used_str[32], avail_str[32];
        df_format_size(fs->total, total_str, sizeof(total_str), human_readable);
        df_format_size(fs->used, used_str, sizeof(used_str), human_readable);
        df_format_size(fs->available, avail_str, sizeof(avail_str), human_readable);
        
        /* Device name (truncate if too long) */
        char device[32];
        if (strlen(fs->device) > 20) {
            snprintf(device, sizeof(device), "%.17s...", fs->device);
        } else {
            strlcpy(device, fs->device, sizeof(device));
        }
        
        /* Inode view (-i): switch columns to inode stats via statvfs. */
        if (inodes) {
            struct statvfs vfs;
            unsigned long long itotal = 0, ifree = 0, iavail = 0, iused = 0;
            int iuse = 0;
            if (statvfs(fs->mountpoint, &vfs) == 0) {
                itotal = (unsigned long long)vfs.f_files;
                ifree  = (unsigned long long)vfs.f_ffree;
                iavail = (unsigned long long)vfs.f_favail;
                iused  = (itotal >= ifree) ? (itotal - ifree) : 0;
                unsigned long long denom = iused + iavail;
                iuse   = denom ? (int)((iused * 100 + denom - 1) / denom) : 0;
            }
            jash_writef(w, ctx, "%-20s %10llu %9llu %9llu %3d%% %s\n",
                        device, itotal, iused, ifree, iuse, fs->mountpoint);
            continue;
        }

        /* Print row */
        if (piped && !force_color) {
            /* Plain output */
            jash_writef(w, ctx, "%-20s ", device);
            if (show_type) jash_writef(w, ctx, "%-9.9s ", fs->fstype);
            if (human_readable) {
                jash_writef(w, ctx, "%5s %5s %5s %3d%% %s\n",
                           total_str, used_str, avail_str,
                           fs->use_percent, fs->mountpoint);
            } else {
                jash_writef(w, ctx, "%10llu %10llu %10llu %3d%% %s\n",
                           fs->total / 1024, fs->used / 1024,
                           fs->available / 1024, fs->use_percent, fs->mountpoint);
            }
        } else {
            /* Colored output with bar */
            const char* color = df_get_color(fs->use_percent, piped, force_color);

            jash_writef(w, ctx, "%-20s ", device);
            if (show_type) jash_writef(w, ctx, "%-9.9s ", fs->fstype);
            
            if (human_readable) {
                jash_writef(w, ctx, "%5s %5s %5s ", total_str, used_str, avail_str);
            } else {
                jash_writef(w, ctx, "%10llu %10llu %10llu ",
                           fs->total / 1024, fs->used / 1024, fs->available / 1024);
            }
            
            /* Unicode bar */
            df_print_bar(fs->use_percent, 20, piped, force_color, w, ctx);
            
            jash_writef(w, ctx, " %s%s%s\n", color, fs->mountpoint, DF_COLOR_RESET);
        }
    }

    /* --total: a grand-total row across all listed filesystems (GNU). */
    if (grand_total && !inodes) {
        int tpct = (tot_used + tot_avail) ?
                   (int)((tot_used * 100 + (tot_used + tot_avail) - 1) / (tot_used + tot_avail)) : 0;
        char ts[32], us[32], as[32];
        df_format_size(tot_total, ts, sizeof ts, human_readable);
        df_format_size(tot_used,  us, sizeof us, human_readable);
        df_format_size(tot_avail, as, sizeof as, human_readable);
        jash_writef(w, ctx, "%-20s ", "total");
        if (show_type) jash_writef(w, ctx, "%-9.9s ", "-");
        if (human_readable)
            jash_writef(w, ctx, "%5s %5s %5s %3d%% -\n", ts, us, as, tpct);
        else
            jash_writef(w, ctx, "%10llu %10llu %10llu %3d%% -\n",
                        tot_total / 1024, tot_used / 1024, tot_avail / 1024, tpct);
    }

    return errors;
}

int jash_b_df_argv(jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv) {
    size_t count = argc > 0 ? (size_t)argc : 1;
    const char** paths = calloc(count, sizeof(*paths));
    df_fs_info_t* selected = calloc(count, sizeof(*selected));
    if (!paths || !selected) {
        free(paths); free(selected);
        jash_writes(jash_stderr_writer, NULL, "df: cannot allocate operand list\n");
        return 1;
    }
    int rc = df_run(s, w, ctx, argc, argv, paths, selected);
    free(paths); free(selected);
    return rc;
}

int jash_b_df(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    jash_argv_t av = {0};
    if (jash_argv_build(args ? args : "", &av) < 0) return 1;
    int rc = jash_b_df_argv(s, w, ctx, av.argc, av.argv);
    jash_argv_free(&av);
    return rc;
}
