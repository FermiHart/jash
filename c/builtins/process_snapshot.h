// SPDX-License-Identifier: LicenseRef-SPL-1.0
#ifndef JASH_PROCESS_SNAPSHOT_H
#define JASH_PROCESS_SNAPSHOT_H
#include "discovery_output.h"
#include "value_output.h"
#include "query_data.h"
#include <core/argv.h>
#include <osif/osinfo.h>
#include <sys/wait.h>
#include <errno.h>

#define PSF(key, value) {JDV_LITERAL(key), (value)}
static jdv_value_t proc_record(jash_discovery_rows_t* rows, const jdv_record_input_t* fields, size_t count) {
    jdv_value_t out = jdv_value_nothing();
    if (jdv_value_record(&rows->arena, fields, count, &out)) rows->error = 1;
    return out;
}
static jdv_value_t proc_list(jash_discovery_rows_t* rows, const jdv_value_t* values, size_t count) {
    jdv_value_t out = jdv_value_nothing();
    if (jdv_value_list(&rows->arena, values, count, &out)) rows->error = 1;
    return out;
}
static int proc_bytes(const char* path, unsigned char** bytes, size_t* count, int private_file) {
    *bytes = NULL; *count = 0;
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC | (private_file ? O_NOFOLLOW : 0));
    if (fd < 0) return -1;
    struct stat before, after;
    if (fstat(fd, &before) || !S_ISREG(before.st_mode) || (private_file == 1 &&
        (before.st_uid != geteuid() || (before.st_mode & 077)))) { close(fd); errno = EACCES; return -1; }
    size_t capacity = 1024u * 1024u;
    unsigned char* memory = malloc(capacity + 1);
    if (!memory) { close(fd); return -1; }
    int error = 0;
    while (*count < capacity + 1) {
        if (jash_query_cancel_poll(NULL)) { error = ECANCELED; break; }
        ssize_t n = read(fd, memory + *count, capacity + 1 - *count);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { error = errno; break; }
        if (!n) break;
        *count += (size_t)n;
    }
    if (*count > capacity) error = EOVERFLOW;
    if (fstat(fd, &after)) error = errno;
    else if (private_file && (before.st_dev != after.st_dev || before.st_ino != after.st_ino ||
        before.st_size != after.st_size || before.st_mtime != after.st_mtime || before.st_ctime != after.st_ctime
#if defined(__linux__)
        || before.st_mtim.tv_nsec != after.st_mtim.tv_nsec || before.st_ctim.tv_nsec != after.st_ctim.tv_nsec
#endif
        )) error = ESTALE;
    if (close(fd) && !error) error = errno;
    if (error) { free(memory); *count = 0; errno = error; return -1; }
    memory[*count] = 0; *bytes = memory; return 0;
}
static jdv_value_t proc_string_vector(jash_discovery_rows_t* rows, const unsigned char* data, size_t count) {
    jdv_value_t values[4096]; size_t n = 0, position = 0;
    if (count && data[count - 1]) { rows->error = 1; return jdv_value_nothing(); }
    while (position < count && !rows->error) {
        if (n == 4096) { rows->error = 1; break; }
        size_t length = strlen((const char*)data + position);
        values[n++] = jash_discovery_string(rows, (const char*)data + position);
        position += length + 1;
    }
    return proc_list(rows, values, n);
}
static int proc_collect_snapshot(jash_discovery_rows_t* rows, int pid, jdv_value_t* out) {
    osif_procinfo_t before, after;
    if (bear_osinfo_proc_get(pid, &before) || !(before.valid & OSIF_PROC_VALID_START)) return -1;
    char path[128]; unsigned char* bytes; size_t size;
    snprintf(path, sizeof path, "/proc/%d/cmdline", pid);
    if (proc_bytes(path, &bytes, &size, 0)) return -1;
    jdv_value_t arguments = proc_string_vector(rows, bytes, size); free(bytes);
    snprintf(path, sizeof path, "/proc/%d/environ", pid);
    int environment_error = 0;
    jdv_value_t environment = jdv_value_nothing();
    if (proc_bytes(path, &bytes, &size, 0)) environment_error = errno;
    else { environment = proc_string_vector(rows, bytes, size); free(bytes); }
    snprintf(path, sizeof path, "/proc/%d/fd", pid);
    DIR* directory = opendir(path);
    int fd_error = directory ? 0 : errno;
    jdv_value_t descriptors[1024]; size_t count = 0;
    while (directory && !rows->error) {
        errno = 0; struct dirent* entry = readdir(directory);
        if (!entry) { if (errno) fd_error = errno; break; }
        if (entry->d_name[0] == '.') continue;
        char* end; errno = 0; unsigned long number = strtoul(entry->d_name, &end, 10);
        if (errno || *end || number > INT_MAX) { fd_error = EINVAL; break; }
        if (count == 1024) { fd_error = EOVERFLOW; break; }
        char target[4096];
        ssize_t n = readlinkat(dirfd(directory), entry->d_name, target, sizeof target - 1);
        if (n < 0) { fd_error = errno; break; }
        if ((size_t)n == sizeof target - 1) { fd_error = ENAMETOOLONG; break; }
        target[n] = 0;
        jdv_record_input_t fields[] = {PSF("fd", jdv_value_u64(number)), PSF("target", jash_discovery_path(rows, target))};
        descriptors[count++] = proc_record(rows, fields, 2);
    }
    if (directory && closedir(directory) && !fd_error) fd_error = errno;
    if (bear_osinfo_proc_get(pid, &after) || !(after.valid & OSIF_PROC_VALID_START) || before.pid != after.pid ||
        before.start_ticks != after.start_ticks) { errno = ESTALE; return -1; }
    jdv_record_input_t tree_fields[] = {
        PSF("pid", jdv_value_u64((uint64_t)pid)), PSF("argv", arguments), PSF("env", environment),
        PSF("fds", fd_error ? jdv_value_nothing() : proc_list(rows, descriptors, count)),
        PSF("children", jdv_value_nothing()), PSF("children_available", jdv_value_bool(0)),
        PSF("start_ticks", (before.valid & OSIF_PROC_VALID_START) ? jdv_value_u64(before.start_ticks) : jdv_value_nothing()),
        PSF("environment_errno", environment_error ? jdv_value_i64(environment_error) : jdv_value_nothing()),
        PSF("descriptors_errno", fd_error ? jdv_value_i64(fd_error) : jdv_value_nothing())
    };
    jdv_value_t tree = proc_record(rows, tree_fields, sizeof tree_fields / sizeof *tree_fields);
    jdv_record_input_t fields[] = {
        PSF("version", jdv_value_u64(3)), PSF("root_pid", jdv_value_u64((uint64_t)pid)),
        PSF("timestamp", jdv_value_i64((int64_t)time(NULL))), PSF("tree", tree),
        PSF("scope", jash_discovery_string(rows, "root-process")),
        PSF("consistency", jash_discovery_string(rows, "process-identity; fields sampled separately")),
        PSF("complete", jdv_value_bool(!environment_error && !fd_error)),
        PSF("restore_scope", jash_discovery_string(rows, "root-argv-only"))
    };
    *out = proc_record(rows, fields, sizeof fields / sizeof *fields);
    return rows->error ? -1 : 0;
}
int jash_process_tree_report(jash_state_t* state, jash_writer_t writer, void* context,
        const char* command, int native, int pid) {
    (void)state;
    int mode = native ? JASH_DISCOVERY_DATA : JASH_DISCOVERY_JSON;
    jash_discovery_rows_t rows;
    if (jash_discovery_rows_init(&rows)) return jash_discovery_error(writer, context, command, mode, "allocation_failed", 125);
    jdv_value_t snapshot;
    int status = proc_collect_snapshot(&rows, pid, &snapshot);
    int cancelled = jash_query_cancel_poll(NULL);
    if (cancelled) {
        if (native) { static const jdv_schema_t any = {.kind=JDV_SCHEMA_ANY}; status = query_data_emit(writer, context, &any, NULL, 0, 0, cancelled, JDV_STREAM_CANCELLED, "request_cancelled"); }
        else status = jash_discovery_error(writer, context, command, mode, "request_cancelled", cancelled);
    }
    else if (status) status = jash_discovery_error(writer, context, command, mode, "process_observation_failed", 1);
    else if (native) {
        static const jdv_schema_t schema = {.kind=JDV_SCHEMA_ANY};
        status = query_data_emit(writer, context, &schema, &snapshot, 1, 0, 0, JDV_STREAM_COMPLETE, NULL);
    } else status = value_output_json(writer, context, command, &snapshot, &JDV_DEFAULT_LIMITS, 32u * 1024u * 1024u, 0);
    jash_discovery_rows_free(&rows); return status;
}
static int proc_exact_pid(const char* value, int* pid) {
    if (!value || !*value || *value < '1' || *value > '9') return -1;
    char* end; errno = 0; long n = strtol(value, &end, 10);
    if (errno || *end || n > INT_MAX) return -1;
    *pid = (int)n; return 0;
}
static int proc_native_dispatch(jash_state_t*, jash_writer_t, void*, int, char**);
#undef PSF
#endif
